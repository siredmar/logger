curl -X POST 192.168.1.66/channel/0/config      -H "Content-Type: application/json" \
     -d '{"samplingInterval":1,"bufferSize":10,"samplingEnabled":true}'
     

curl -X GET 192.168.1.66/channel/0 | jq
