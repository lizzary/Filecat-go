// filecat-watch: minimal demo of the coalesced FileEvent stream.
//
//	go run ./examples/watch
//
// Edit, create, rename, or delete files under the watched directory; the
// program prints one line per FileEvent emitted by the binding's
// Watchman-style coalescer. Rename pairs and remove+create reuse on
// Windows collapse into a single MOVE line.
package main

import (
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/lizzary/filecat-go"
)

const (
	watchDir        = "C:\\Users\\29623\\Downloads"
	recursive       = true
	eventBufferSize = 256
	// Settle window: events arriving within 50ms of the first event in a
	// batch get coalesced together. Matches Watchman's default ballpark.
	coalesceWindow = 50 * time.Millisecond
)

func main() {
	w, err := filecat.NewWatcher(watchDir, recursive, eventBufferSize, coalesceWindow)
	if err != nil {
		log.Fatalf("open: %v", err)
	}
	defer func() { _ = w.Close() }()

	go func() {
		for err := range w.Errors() {
			log.Printf("[error] %v", err)
		}
	}()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stop
		_ = w.Close()
	}()

	log.Printf("watching %s (recursive=%v, window=%s). Ctrl+C to stop.",
		watchDir, recursive, coalesceWindow)

	for ev := range w.Events() {
		switch ev.Type {
		case filecat.EventMove:
			fmt.Printf("%-8s %s -> %s\n", ev.Type, ev.OldPath, ev.Path)
		default:
			fmt.Printf("%-8s %s\n", ev.Type, ev.Path)
		}
	}
}
