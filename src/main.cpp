// src/main.cpp
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "credentials.h"

// HTTP server on port 80
WebServer server(80);
// WebSocket server on port 81
WebSocketsServer ws(81);

// Preferences (NVS) for WiFi config
Preferences prefs;
enum
{
  MODE_STA = 0,
  MODE_AP = 1
};
const char *PREF_NAMESPACE = "wifi";
const char *KEY_MODE = "mode";
const char *KEY_SSID = "ssid";
const char *KEY_PASS = "pass";

// Default soft-AP credentials
const char *DEFAULT_AP_SSID = "ESP32-AP";
const char *DEFAULT_AP_PASS = "config123";

// ADC & buffer parameters
#define MAX_CHANNELS 4
#define MAX_BUFFER_SIZE 100
const uint8_t adcPins[MAX_CHANNELS] = {34, 35, 32, 33};

struct Sample
{
  uint32_t timestamp;
  uint32_t value;
};

struct Channel
{
  bool configured = false;
  bool samplingEnabled = false;
  uint32_t samplingInterval = 1000; // ms
  uint32_t lastSampleTime = 0;
  uint16_t bufferSize = 0;
  uint16_t head = 0;
  uint16_t tail = 0;
  uint16_t count = 0;
  bool overflow = false;
  Sample buffer[MAX_BUFFER_SIZE];
};

Channel channels[MAX_CHANNELS];

// --- WiFi setup ---
void setupWiFi()
{
  prefs.begin(PREF_NAMESPACE, false);
  uint8_t mode = prefs.getUInt(KEY_MODE, MODE_AP);

  // enable both AP and STA
  WiFi.mode(WIFI_AP_STA);

  // start soft-AP
  WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
  Serial.printf("AP up: SSID=%s, IP=%s\n",
                DEFAULT_AP_SSID,
                WiFi.softAPIP().toString().c_str());

  // if STA mode selected, attempt to join configured network
  if (mode == MODE_STA)
  {
    String ss = prefs.getString(KEY_SSID, "");
    String pw = prefs.getString(KEY_PASS, "");
    if (ss.length())
    {
      WiFi.begin(ss.c_str(), pw.c_str());
      Serial.printf("Joining STA '%s'… ", ss.c_str());
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000)
      {
        delay(500);
        Serial.print('.');
      }
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.printf("\nSTA IP: %s\n", WiFi.localIP().toString().c_str());
      }
      else
      {
        Serial.println("\nFailed to join STA, staying AP only");
      }
    }
  }
}

// --- HTTP handlers for WiFi config ---
void handleGetWiFi()
{
  StaticJsonDocument<256> doc;
  uint8_t mode = prefs.getUInt(KEY_MODE, MODE_AP);
  doc["mode"] = (mode == MODE_STA ? "sta" : "ap");
  if (mode == MODE_STA)
  {
    doc["ssid"] = prefs.getString(KEY_SSID, "");
  }
  else
  {
    doc["ap_ssid"] = DEFAULT_AP_SSID;
    doc["ap_pass"] = DEFAULT_AP_PASS;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSetWiFi()
{
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err)
  {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  const char *m = doc["mode"];
  if (!m || (strcmp(m, "sta") && strcmp(m, "ap")))
  {
    server.send(400, "application/json", "{\"error\":\"mode must be 'sta' or 'ap'\"}");
    return;
  }
  prefs.putUInt(KEY_MODE, strcmp(m, "sta") == 0 ? MODE_STA : MODE_AP);
  if (strcmp(m, "sta") == 0)
  {
    const char *ss = doc["ssid"];
    const char *pw = doc["pass"];
    if (!ss || !pw)
    {
      server.send(400, "application/json", "{\"error\":\"sta requires ssid & pass\"}");
      return;
    }
    prefs.putString(KEY_SSID, ss);
    prefs.putString(KEY_PASS, pw);
  }
  server.send(200, "application/json", "{\"status\":\"OK, restarting\"}");
  delay(500);
  ESP.restart();
}

// --- Utility to extract channel from URI ---
bool extractChannel(const String &uri, int &ch)
{
  int start = uri.indexOf("/channel/") + 9;
  int end = uri.indexOf('/', start);
  String s = (end < 0 ? uri.substring(start) : uri.substring(start, end));
  ch = s.toInt();
  return s.length() > 0 && ch >= 0 && ch < MAX_CHANNELS;
}

// --- HTTP handlers for channel config & data ---
void handleGetConfig()
{
  int ch;
  String uri = server.uri();
  if (!extractChannel(uri, ch) || !channels[ch].configured)
  {
    server.send(404, "application/json", "{\"error\":\"Channel not configured\"}");
    return;
  }
  StaticJsonDocument<256> doc;
  auto &C = channels[ch];
  doc["samplingInterval"] = C.samplingInterval / 1000;
  doc["bufferSize"] = C.bufferSize;
  doc["samplingEnabled"] = C.samplingEnabled;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSetConfig()
{
  int ch;
  String uri = server.uri();
  if (!extractChannel(uri, ch))
  {
    server.send(400, "application/json", "{\"error\":\"Invalid channel\"}");
    return;
  }
  String payload = server.arg("plain");
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err)
  {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  uint32_t intervalMs = doc["samplingInterval"].as<uint32_t>() * 1000;
  uint16_t bufSize = doc["bufferSize"].as<uint16_t>();
  bool enabled = doc["samplingEnabled"].as<bool>();
  if (bufSize == 0 || bufSize > MAX_BUFFER_SIZE)
  {
    server.send(400, "application/json", "{\"error\":\"bufferSize out of range\"}");
    return;
  }
  auto &C = channels[ch];
  C.configured = true;
  C.samplingInterval = intervalMs;
  C.bufferSize = bufSize;
  C.samplingEnabled = enabled;
  C.head = C.tail = C.count = 0;
  C.overflow = false;
  C.lastSampleTime = millis();
  server.send(200, "application/json", "{\"status\":\"OK\"}");
}

void handleGetData()
{
  int ch;
  String uri = server.uri();
  if (!extractChannel(uri, ch) || !channels[ch].configured)
  {
    server.send(404, "application/json", "{\"error\":\"Channel not configured\"}");
    return;
  }
  auto &C = channels[ch];
  StaticJsonDocument<1024> doc;
  auto arr = doc.createNestedArray("data");
  for (uint16_t i = 0; i < C.count; i++)
  {
    uint16_t idx = (C.tail + i) % C.bufferSize;
    auto &s = C.buffer[idx];
    JsonObject obj = arr.createNestedObject();
    obj["timestamp"] = s.timestamp;
    obj["value"] = s.value;
  }
  doc["overflow"] = C.overflow;
  C.tail = (C.tail + C.count) % C.bufferSize;
  C.count = 0;
  C.overflow = false;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// --- WebSocket handlers ---
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t *, size_t)
{
  if (type == WStype_CONNECTED)
  {
    ws.sendTXT(num, "{\"msg\":\"WebSocket connected\"}");
  }
}

void broadcastSample(uint8_t channel, const Sample &s)
{
  StaticJsonDocument<128> doc;
  doc["channel"] = channel;
  doc["timestamp"] = s.timestamp;
  doc["value"] = s.value;
  String out;
  serializeJson(doc, out);
  ws.broadcastTXT(out);
}

void setup()
{
  Serial.begin(115200);
  setupWiFi();

  // WiFi config API
  server.on("/wifi", HTTP_GET, handleGetWiFi);
  server.on("/wifi", HTTP_POST, handleSetWiFi);

  // Channel APIs
  for (int i = 0; i < MAX_CHANNELS; i++)
  {
    String cfg = "/channel/" + String(i) + "/config";
    String dat = "/channel/" + String(i);
    server.on(cfg, HTTP_GET, handleGetConfig);
    server.on(cfg, HTTP_POST, handleSetConfig);
    server.on(dat, HTTP_GET, handleGetData);
  }
  server.begin();

  // WebSocket
  ws.begin();
  ws.onEvent(onWebSocketEvent);

  Serial.println("HTTP + WebSocket servers started");
}

void loop()
{
  uint32_t now = millis();
  ws.loop();

  // sample ADC, buffer & broadcast
  for (int i = 0; i < MAX_CHANNELS; i++)
  {
    auto &C = channels[i];
    if (!C.configured || !C.samplingEnabled)
      continue;
    if (now - C.lastSampleTime < C.samplingInterval)
      continue;

    Sample s{now, analogRead(adcPins[i])};

    if (C.count < C.bufferSize)
    {
      C.count++;
    }
    else
    {
      C.overflow = true;
      C.tail = (C.tail + 1) % C.bufferSize;
    }

    C.buffer[C.head] = s;
    broadcastSample(i, s);
    C.head = (C.head + 1) % C.bufferSize;
    C.lastSampleTime = now;

    Serial.printf("Sample ch %d -> %u, %u\n", i, s.timestamp, s.value);
  }

  server.handleClient();
}
