// filecat-watch: tiny demo that mirrors the upstream C example.
//
//	go run ./examples/watch <path> [recursive=0|1]
package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"syscall"

	"github.com/lizzary/filecat-go"
)

func main() {
	if len(os.Args) < 2 || len(os.Args) > 3 {
		fmt.Fprintf(os.Stderr, "usage: %s <path> [recursive=0|1]\n", os.Args[0])
		os.Exit(2)
	}
	path := os.Args[1]
	recursive := false
	if len(os.Args) == 3 {
		n, err := strconv.Atoi(os.Args[2])
		if err != nil {
			fmt.Fprintf(os.Stderr, "recursive must be 0 or 1: %v\n", err)
			os.Exit(2)
		}
		recursive = n != 0
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	w, err := filecat.Open(ctx, path, recursive)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer w.Close()

	fmt.Fprintf(os.Stderr, "watching %s (recursive=%v). Ctrl+C to stop.\n", path, recursive)

	for {
		select {
		case ev, ok := <-w.Events():
			if !ok {
				if err := w.Close(); err != nil {
					fmt.Fprintf(os.Stderr, "** %v\n", err)
					os.Exit(1)
				}
				return
			}
			fmt.Printf("%-12s %s\n", ev.Type, ev.Path)
		case err, ok := <-w.Errors():
			if !ok {
				continue
			}
			if errors.Is(err, filecat.ErrOverflow) {
				fmt.Fprintln(os.Stderr, "** overflow: events were dropped")
			} else {
				fmt.Fprintf(os.Stderr, "** %v\n", err)
			}
		}
	}
}
