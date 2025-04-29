// src/main.cpp
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "credentials.h"

// HTTP server on port 80
WebServer server(80);

// WebSocket server on port 81
WebSocketsServer ws(81);

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

bool extractChannel(const String &uri, int &ch)
{
  int start = uri.indexOf("/channel/") + 9;
  int end = uri.indexOf('/', start);
  String numStr = end < 0 ? uri.substring(start) : uri.substring(start, end);
  ch = numStr.toInt();
  return numStr.length() > 0 && ch >= 0 && ch < MAX_CHANNELS;
}

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
  doc["samplingInterval"] = channels[ch].samplingInterval / 1000;
  doc["bufferSize"] = channels[ch].bufferSize;
  doc["samplingEnabled"] = channels[ch].samplingEnabled;
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
  if (deserializeJson(doc, payload))
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
  Channel &C = channels[ch];
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
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\nConnected, IP: %s\n", WiFi.localIP().toString().c_str());

  analogReadResolution(12);
  for (int i = 0; i < MAX_CHANNELS; i++)
  {
    analogSetPinAttenuation(adcPins[i], ADC_11db);
  }

  for (int i = 0; i < MAX_CHANNELS; i++)
  {
    String cfg = "/channel/" + String(i) + "/config";
    String dat = "/channel/" + String(i);
    server.on(cfg, HTTP_GET, handleGetConfig);
    server.on(cfg, HTTP_POST, handleSetConfig);
    server.on(dat, HTTP_GET, handleGetData);
  }
  server.begin();

  ws.begin();
  ws.onEvent(onWebSocketEvent);

  Serial.println("HTTP + WebSocket servers started");
}

void loop()
{
  uint32_t now = millis();
  ws.loop();
  for (int i = 0; i < MAX_CHANNELS; i++)
  {
    Channel &C = channels[i];
    if (!C.configured || !C.samplingEnabled)
      continue;
    if (now - C.lastSampleTime < C.samplingInterval)
      continue;

    uint32_t ts = now;
    uint32_t val = analogRead(adcPins[i]);

    Sample s{ts, val};

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

    Serial.printf("Sample ch %d -> %u, %u\n", i, ts, val);
  }
  server.handleClient();
}
