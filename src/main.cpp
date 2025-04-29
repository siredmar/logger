#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "credentials.h" // defines const char* ssid, password

// HTTP server on port 80
WebServer server(80);

// Ring buffer parameters
#define MAX_CHANNELS 4
#define MAX_BUFFER_SIZE 100

// Map logical channels to ADC1 pins to avoid ADC2/Wi-Fi conflicts
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
  uint16_t head = 0;     // next write index
  uint16_t tail = 0;     // next read index
  uint16_t count = 0;    // unread samples count
  bool overflow = false; // overflow since last read
  Sample buffer[MAX_BUFFER_SIZE];
};

Channel channels[MAX_CHANNELS];

// Utility: parse channel number from URI
bool extractChannel(const String &uri, int &ch)
{
  int start = uri.indexOf("/channel/") + 9;
  int end = uri.indexOf('/', start);
  String numStr = (end < 0 ? uri.substring(start) : uri.substring(start, end));
  ch = numStr.toInt();
  return (numStr.length() > 0 && ch >= 0 && ch < MAX_CHANNELS);
}

// GET /channel/<n>/config
void handleGetConfig()
{
  int ch;
  String uri = server.uri();
  if (!extractChannel(uri, ch) || !channels[ch].configured)
  {
    Serial.printf("GET %s -> 404\n", uri.c_str());
    server.send(404, "application/json", "{\"error\":\"Channel not configured\"}");
    return;
  }
  Serial.printf("GET %s\n", uri.c_str());
  StaticJsonDocument<256> doc;
  doc["samplingInterval"] = channels[ch].samplingInterval / 1000;
  doc["bufferSize"] = channels[ch].bufferSize;
  doc["samplingEnabled"] = channels[ch].samplingEnabled;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /channel/<n>/config
void handleSetConfig()
{
  int ch;
  String uri = server.uri();
  if (!extractChannel(uri, ch))
  {
    Serial.printf("POST %s -> 400\n", uri.c_str());
    server.send(400, "application/json", "{\"error\":\"Invalid channel\"}");
    return;
  }
  String payload = server.arg("plain");
  Serial.printf("POST %s -> payload: %s\n", uri.c_str(), payload.c_str());
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err)
  {
    Serial.printf("JSON error %s: %s\n", uri.c_str(), err.c_str());
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  uint32_t interval = doc["samplingInterval"].as<uint32_t>() * 1000;
  uint16_t bufSize = doc["bufferSize"].as<uint16_t>();
  bool enabled = doc["samplingEnabled"].as<bool>();
  if (bufSize == 0 || bufSize > MAX_BUFFER_SIZE)
  {
    server.send(400, "application/json", "{\"error\":\"bufferSize out of range\"}");
    return;
  }
  channels[ch].configured = true;
  channels[ch].samplingInterval = interval;
  channels[ch].bufferSize = bufSize;
  channels[ch].samplingEnabled = enabled;
  channels[ch].head = 0;
  channels[ch].tail = 0;
  channels[ch].count = 0;
  channels[ch].overflow = false;
  channels[ch].lastSampleTime = millis();
  Serial.printf("Channel %d configured: interval=%u ms, bufferSize=%u, enabled=%d\n", ch, interval, bufSize, enabled);
  server.send(200, "application/json", "{\"status\":\"OK\"}");
}

// GET /channel/<n>
void handleGetData()
{
  int ch;
  String uri = server.uri();
  if (!extractChannel(uri, ch) || !channels[ch].configured)
  {
    Serial.printf("GET %s -> 404\n", uri.c_str());
    server.send(404, "application/json", "{\"error\":\"Channel not configured\"}");
    return;
  }
  Channel &C = channels[ch];
  Serial.printf("GET %s -> unread: %u\n", uri.c_str(), C.count);
  StaticJsonDocument<1024> doc;
  auto arr = doc.createNestedArray("data");
  for (uint16_t i = 0; i < C.count; i++)
  {
    uint16_t idx = (C.tail + i) % C.bufferSize;
    JsonObject obj = arr.createNestedObject();
    obj["timestamp"] = C.buffer[idx].timestamp;
    obj["value"] = C.buffer[idx].value;
  }
  doc["overflow"] = C.overflow;
  C.tail = (C.tail + C.count) % C.bufferSize;
  C.count = 0;
  C.overflow = false;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void setup()
{
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.printf("Connected, IP: %s\n", WiFi.localIP().toString().c_str());

  // Configure ADC
  analogReadResolution(12);
  for (int i = 0; i < MAX_CHANNELS; i++)
  {
    analogSetPinAttenuation(adcPins[i], ADC_11db);
  }

  // Register REST endpoints
  for (int i = 0; i < MAX_CHANNELS; i++)
  {
    String cfgPath = String("/channel/") + i + "/config";
    server.on(cfgPath, HTTP_GET, handleGetConfig);
    server.on(cfgPath, HTTP_POST, handleSetConfig);
    String dataPath = String("/channel/") + i;
    server.on(dataPath, HTTP_GET, handleGetData);
  }

  server.begin();
  Serial.println("HTTP server started");
}

void loop()
{
  uint32_t now = millis();
  for (int i = 0; i < MAX_CHANNELS; i++)
  {
    Channel &C = channels[i];
    if (C.configured && C.samplingEnabled && now - C.lastSampleTime >= C.samplingInterval)
    {
      uint32_t ts = now;
      uint32_t val = analogRead(adcPins[i]);
      if (C.count < C.bufferSize)
      {
        C.count++;
      }
      else
      {
        C.overflow = true;
        C.tail = (C.tail + 1) % C.bufferSize;
      }
      C.buffer[C.head] = {ts, val};
      Serial.printf("Sample ch %d -> %u, %u\n", i, ts, val);
      C.head = (C.head + 1) % C.bufferSize;
      C.lastSampleTime = now;
    }
  }
  server.handleClient();
}
