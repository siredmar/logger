package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"

	"github.com/gorilla/websocket"
)

type Sample struct {
	Channel   int     `json:"channel"`
	Timestamp uint32  `json:"timestamp"`
	Value     float32 `json:"value"`
}

func streamSamples(wsURL string) (<-chan Sample, <-chan error) {
	samples := make(chan Sample)
	errs := make(chan error, 1)

	go func() {
		defer close(samples)
		defer close(errs)

		conn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
		if err != nil {
			errs <- err
			return
		}
		defer conn.Close()

		for {
			_, msg, err := conn.ReadMessage()
			if err != nil {
				errs <- err
				return
			}
			var s Sample
			if err := json.Unmarshal(msg, &s); err != nil {
				log.Printf("JSON unmarshal error: %v", err)
				continue
			}
			samples <- s
		}
	}()

	return samples, errs
}

func main() {
	host := flag.String("host", "192.168.1.66", "ESP32 WebSocket server host")
	port := flag.Int("port", 81, "WebSocket server port")
	flag.Parse()

	wsURL := fmt.Sprintf("ws://%s:%d", *host, *port)
	samples, errs := streamSamples(wsURL)

	for {
		select {
		case s, ok := <-samples:
			if !ok {
				return
			}
			fmt.Printf("%d, %d, %f\n", s.Channel, s.Timestamp, s.Value)
		case err := <-errs:
			log.Fatalf("stream error: %v", err)
		}
	}
}
