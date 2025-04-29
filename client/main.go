package main

import (
    "encoding/json"
    "log"
    "net/url"
    "os"
    "os/signal"

    "github.com/gorilla/websocket"
)

type Sample struct {
    Timestamp uint32 `json:"timestamp"`
    Value     uint32 `json:"value"`
}

func main() {
    // connect to ESP32 WebSocket endpoint (you’d add this to your firmware)
    u := url.URL{Scheme: "ws", Host: "192.168.1.66", Path: "/ws/channel/0"}
    conn, _, err := websocket.DefaultDialer.Dial(u.String(), nil)
    if err != nil {
        log.Fatal("dial:", err)
    }
    defer conn.Close()

    // channel for incoming samples
    samples := make(chan Sample)

    // reader goroutine
    go func() {
        defer close(samples)
        for {
            _, msg, err := conn.ReadMessage()
            if err != nil {
                log.Println("read:", err)
                return
            }
            var s Sample
            if err := json.Unmarshal(msg, &s); err != nil {
                log.Println("unmarshal:", err)
                continue
            }
            samples <- s
        }
    }()

    // signal handling for clean shutdown
    interrupt := make(chan os.Signal, 1)
    signal.Notify(interrupt, os.Interrupt)

    // consume samples
    for {
        select {
        case s, ok := <-samples:
            if !ok {
                log.Println("stream closed")
                return
            }
            log.Printf("got sample: %#v\n", s)
        case <-interrupt:
            log.Println("interrupt, closing")
            conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
            return
        }
    }
}
