// src/main.cpp
#include "credentials.h"
#include <ArduinoJson.h>
#include <PicoMQTT.h>
#include <PicoWebsocket.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

// HTTP server on port 80
WebServer server(80);
// WebSocket server on port 81
WebSocketsServer ws(81);

// MQTT TCP server on 1883
WiFiServer mqttTcpServer(1883);
// MQTT WebSocket server on port 82
WiFiServer mqttWebsocketServerUnderlying(82);
PicoWebsocket::Server<WiFiServer>
    mqttWebsocketServer(mqttWebsocketServerUnderlying);

// Subclass PicoMQTT::Server to get virtual hooks
class DebugMQTTServer : public PicoMQTT::Server {
public:
  DebugMQTTServer(WiFiServer &tcp, PicoWebsocket::Server<WiFiServer> &ws)
      : PicoMQTT::Server(tcp, ws) {}
  void on_connected(const char *client_id) override {
    Serial.printf("MQTT CONNECT clientId=%s\n", client_id);
  }
  void on_disconnected(const char *client_id) override {
    Serial.printf("MQTT DISCONNECT clientId=%s\n", client_id);
  }
  void on_subscribe(const char *client_id, const char *topic) override {
    Serial.printf("MQTT SUBSCRIBE clientId=%s topic=%s\n", client_id, topic);
  }
  void on_unsubscribe(const char *client_id, const char *topic) override {
    Serial.printf("MQTT UNSUBSCRIBE clientId=%s topic=%s\n", client_id, topic);
  }
};

// PicoMQTT server instance
DebugMQTTServer mqtt(mqttTcpServer, mqttWebsocketServer);

// Preferences (NVS) for WiFi and channels
Preferences prefs;
enum { MODE_STA = 0, MODE_AP = 1 };
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

struct Sample {
  uint32_t timestamp;
  float value;
};

struct Channel {
  bool configured = false;
  bool samplingEnabled = false;
  uint32_t samplingInterval = 1000; // ms
  uint32_t lastSampleTime = 0;
  uint16_t bufferSize = 0;
  uint16_t head = 0;
  uint16_t tail = 0;
  uint16_t count = 0;
  // calibration parameters
  float offset = 0.0f; // volts
  float factor = 1.0f;
  float divisor = 1.0f;
  bool overflow = false;
  Sample buffer[MAX_BUFFER_SIZE];
};
Channel channels[MAX_CHANNELS];

// NVS key helper: CH_KEY(channel, name)
#define CH_KEY(ch, name) (String("ch") + ch + "_" + name)

// Load per-channel configuration from NVS
void loadChannelConfigs() {
  prefs.begin(PREF_NAMESPACE, false);
  for (int i = 0; i < MAX_CHANNELS; i++) {
    String kCfg = CH_KEY(i, "cfg");
    if (!prefs.getBool(kCfg.c_str(), false))
      continue;
    Channel &C = channels[i];
    C.configured = true;
    C.samplingEnabled =
        prefs.getBool(CH_KEY(i, "enabled").c_str(), C.samplingEnabled);
    C.samplingInterval =
        prefs.getUInt(CH_KEY(i, "interval").c_str(), C.samplingInterval);
    C.bufferSize = prefs.getUInt(CH_KEY(i, "bufsize").c_str(), C.bufferSize);
    C.offset = prefs.getFloat(CH_KEY(i, "offset").c_str(), C.offset);
    C.factor = prefs.getFloat(CH_KEY(i, "factor").c_str(), C.factor);
    C.divisor = prefs.getFloat(CH_KEY(i, "divisor").c_str(), C.divisor);
    C.head = C.tail = C.count = 0;
    C.overflow = false;
    C.lastSampleTime = millis();
  }
}

// --- WiFi setup ---
void setupWiFi() {
  prefs.begin(PREF_NAMESPACE, false);
  uint8_t mode = prefs.getUInt(KEY_MODE, MODE_AP);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
  Serial.printf("AP up: SSID=%s, IP=%s\n", DEFAULT_AP_SSID,
                WiFi.softAPIP().toString().c_str());
  if (mode == MODE_STA) {
    String ss = prefs.getString(KEY_SSID, "");
    String pw = prefs.getString(KEY_PASS, "");
    if (ss.length()) {
      WiFi.begin(ss.c_str(), pw.c_str());
      Serial.printf("Joining STA '%s'… ", ss.c_str());
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        delay(500);
        Serial.print('.');
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nSTA IP: %s\n", WiFi.localIP().toString().c_str());
      } else {
        Serial.println("\nFailed to join STA, staying AP only");
      }
    }
  }
}

// --- HTTP handlers for WiFi config ---
void handleGetWiFi() {
  StaticJsonDocument<256> doc;
  uint8_t mode = prefs.getUInt(KEY_MODE, MODE_AP);
  doc["mode"] = (mode == MODE_STA ? "sta" : "ap");
  if (mode == MODE_STA)
    doc["ssid"] = prefs.getString(KEY_SSID, "");
  else {
    doc["ap_ssid"] = DEFAULT_AP_SSID;
    doc["ap_pass"] = DEFAULT_AP_PASS;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSetWiFi() {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  const char *m = doc["mode"];
  if (!m || (strcmp(m, "sta") && strcmp(m, "ap"))) {
    server.send(400, "application/json",
                "{\"error\":\"mode must be 'sta' or 'ap'\"}");
    return;
  }
  prefs.putUInt(KEY_MODE, strcmp(m, "sta") == 0 ? MODE_STA : MODE_AP);
  if (strcmp(m, "sta") == 0) {
    const char *ss = doc["ssid"], *pw = doc["pass"];
    if (!ss || !pw) {
      server.send(400, "application/json",
                  "{\"error\":\"sta requires ssid & pass\"}");
      return;
    }
    prefs.putString(KEY_SSID, ss);
    prefs.putString(KEY_PASS, pw);
  }
  server.send(200, "application/json", "{\"status\":\"OK, restarting\"}");
  delay(500);
  ESP.restart();
}

// --- Utility to extract channel number ---
bool extractChannel(const String &uri, int &ch) {
  int start = uri.indexOf("/channel/") + 9;
  int end = uri.indexOf('/', start);
  String s = (end < 0 ? uri.substring(start) : uri.substring(start, end));
  ch = s.toInt();
  return s.length() > 0 && ch >= 0 && ch < MAX_CHANNELS;
}

// --- HTTP handlers for channel config/data ---
void handleGetConfig() {
  int ch;
  String uri = server.uri();
  if (!extractChannel(uri, ch) || !channels[ch].configured) {
    server.send(404, "application/json",
                "{\"error\":\"Channel not configured\"}");
    return;
  }
  auto &C = channels[ch];
  StaticJsonDocument<256> doc;
  doc["samplingInterval"] = C.samplingInterval / 1000;
  doc["bufferSize"] = C.bufferSize;
  doc["samplingEnabled"] = C.samplingEnabled;
  doc["offset"] = C.offset;
  doc["factor"] = C.factor;
  doc["divisor"] = C.divisor;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSetConfig() {
  int ch;
  String uri = server.uri();
  if (!extractChannel(uri, ch)) {
    server.send(400, "application/json", "{\"error\":\"Invalid channel\"}");
    return;
  }
  StaticJsonDocument<256> doc;
  deserializeJson(doc, server.arg("plain"));
  uint32_t intervalMs = doc["samplingInterval"].as<uint32_t>() * 1000;
  uint16_t bufSize = doc["bufferSize"].as<uint16_t>();
  bool enabled = doc["samplingEnabled"].as<bool>();
  float offset = doc["offset"].as<float>();
  float factor = doc["factor"].as<float>();
  float divisor = doc["divisor"].as<float>();
  if (bufSize == 0 || bufSize > MAX_BUFFER_SIZE) {
    server.send(400, "application/json",
                "{\"error\":\"bufferSize out of range\"}");
    return;
  }
  Channel &C = channels[ch];
  C.configured = C.samplingEnabled = true;
  C.samplingInterval = intervalMs;
  C.bufferSize = bufSize;
  C.samplingEnabled = enabled;
  C.offset = offset;
  C.factor = factor;
  C.divisor = divisor;
  C.head = C.tail = C.count = 0;
  C.overflow = false;
  C.lastSampleTime = millis();
  // persist
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putBool(CH_KEY(ch, "cfg").c_str(), true);
  prefs.putBool(CH_KEY(ch, "enabled").c_str(), enabled);
  prefs.putUInt(CH_KEY(ch, "interval").c_str(), intervalMs);
  prefs.putUInt(CH_KEY(ch, "bufsize").c_str(), bufSize);
  prefs.putFloat(CH_KEY(ch, "offset").c_str(), offset);
  prefs.putFloat(CH_KEY(ch, "factor").c_str(), factor);
  prefs.putFloat(CH_KEY(ch, "divisor").c_str(), divisor);
  server.send(200, "application/json", "{\"status\":\"OK\"}");
}

void handleGetData() { /* unchanged HTTP data handler */ }

// --- WebSocket handlers ---
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t *, size_t) {
  if (type == WStype_CONNECTED)
    ws.sendTXT(num, "{\"msg\":\"WebSocket connected\"}");
}

void broadcastSample(uint8_t channel, const Sample &s) {
  StaticJsonDocument<128> d;
  d["channel"] = channel;
  d["timestamp"] = s.timestamp;
  d["value"] = s.value;
  String o;
  serializeJson(d, o);
  ws.broadcastTXT(o);
}

void setup() {
  Serial.begin(115200);
  setupWiFi();
  loadChannelConfigs();
  server.on("/wifi", HTTP_GET, handleGetWiFi);
  server.on("/wifi", HTTP_POST, handleSetWiFi);
  for (int i = 0; i < MAX_CHANNELS; i++) {
    String cfg = "/channel/" + String(i) + "/config";
    String dat = "/channel/" + String(i);
    server.on(cfg, HTTP_GET, handleGetConfig);
    server.on(cfg, HTTP_POST, handleSetConfig);
    server.on(dat, HTTP_GET, handleGetData);
  }
  server.begin();
  ws.begin();
  ws.onEvent(onWebSocketEvent);
  mqtt.begin();
  Serial.println("HTTP+WS+MQTT started");
}

void loop() {
  uint32_t now = millis();
  ws.loop();
  for (int i = 0; i < MAX_CHANNELS; i++) {
    auto &C = channels[i];
    if (!C.configured || !C.samplingEnabled)
      continue;
    if (now - C.lastSampleTime < C.samplingInterval)
      continue;
    uint32_t raw = analogRead(adcPins[i]);
    float sv = raw / 4096.0f * 3.3f;
    float cv = sv - C.offset;
    float m = cv * C.factor / C.divisor;
    Sample s{now, m};
    if (C.count < C.bufferSize)
      C.count++;
    else {
      C.overflow = true;
      C.tail = (C.tail + 1) % C.bufferSize;
    }
    C.buffer[C.head] = s;
    broadcastSample(i, s);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "{%.4f}", m);
    mqtt.publish((String("channel/") + i).c_str(), (uint8_t *)buf, len);
    C.head = (C.head + 1) % C.bufferSize;
    C.lastSampleTime = now;
    Serial.printf("Sample ch %d -> %u, %.04f\n", i, s.timestamp, s.value);
  }
  server.handleClient();
  mqtt.loop();
}
