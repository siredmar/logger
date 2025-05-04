# logger

This is a NodeMCU (ESP-WROOM-32) based ADC data logger.

## Building

Install PlatformIO in Visual Studio code, hit compile and upload. 

## Updating

Upload via USB cable or use the OTA mechanism. For this the computer must be connected to the ESP somehow via Wifi (AP or STA mode).

## Configuration

### Channel config

```
# write config
curl -X POST 192.168.1.66/channel/1/config\
     -H "Content-Type: application/json" \
     -d '{"samplingInterval":1,"bufferSize":1,"samplingEnabled":true,"offset":0.194,"factor":82.0,"divisor":1.0, "fl":80}'
# read config
curl -X GET 192.168.1.66/channel/1/config | jq
```

## Wifi config

```
# write wifi config to connect to WIFI
curl -X POST http://192.168.4.1/wifi -H "Content-Type: application/json" -d '{"mode":"sta","ssid":"myWIFI","pass":"mypassword"}'
# write wifi config to make a custom WIFI
curl -X POST http://192.168.4.1/wifi -H "Content-Type: application/json" -d '{"mode":"ap"}'
# Read wifi config
curl http://192.168.4.1/wifi | jq
```

## Reading Samples

## Using HTTP Requests

```
curl -s -X GET 192.168.1.66/channel/1 | jq
{
  "data": [
    {
      "timestamp": 184537436,
      "value": 84.57603
    }
  ],
  "overflow": true
}
```

## Using MQTT

```
$ mosquitto_sub -h 192.168.1.66 -t 'channel/1' -v
channel/1 {84.4439}
channel/1 {84.5100}
channel/1 {84.5100}
channel/1 {84.5100}
```

## Using Websockets

See `client/` for a golang demo accessing the measurement channels via websocket in golang

```
$ go run main.go -host 192.168.1.66
0, 0, 0.000000
1, 184795439, 84.443909
1, 184796439, 84.443909
1, 184797439, 84.509972
1, 184798439, 84.377838
1, 184799439, 84.245697
```
