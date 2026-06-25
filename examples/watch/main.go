// filecat-watch: tiny demo that mirrors the upstream C example.
//
//	go run ./examples/watch <path> [recursive=0|1]
package main

import (
	"fmt"

	"github.com/lizzary/filecat-go/internal/cbinding"
)

func main() {
	w, _ := cbinding.Open("C:\\Users\\29623\\Downloads", true)
	for {
		evtype, path, _ := w.NextEvent()
		fmt.Printf("Event type: %d path: %s \n", evtype, path)
	}
}
