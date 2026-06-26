// Package filecat provides cross-platform recursive directory watching.
// It is a Go wrapper over the Filecat C library that adds Watchman-style
// event coalescing: rename pairs are synthesized into Move events, modify
// storms are deduplicated per path, and on macOS the binding disambiguates
// rename direction by checking which side of the pair still exists on disk.
package filecat

import (
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/lizzary/filecat-go/internal/cbinding"
)

type EventType int32

const (
	EventCreated  EventType = 1
	EventRemoved  EventType = 2
	EventModified EventType = 3
	EventMove     EventType = 4
)

func (t EventType) String() string {
	switch t {
	case EventCreated:
		return "Created"
	case EventRemoved:
		return "Removed"
	case EventModified:
		return "Modified"
	case EventMove:
		return "Move"
	}
	return fmt.Sprintf("EventType(%d)", t)
}

// FileEvent is the coalesced event emitted on Watcher.Events().
//
// Path always holds the affected path. For EventMove it is the destination
// (the rename target); OldPath holds the source. For every other type
// OldPath is empty.
//
// Paths are absolute (the watch root is prepended to the relative path the
// C library returns).
type FileEvent struct {
	Type    EventType
	Path    string
	OldPath string
}

// ErrOverflow is reported on Watcher.Errors() when the underlying kernel
// event buffer overflowed. The watcher remains valid; some events were
// dropped and are not recoverable.
var ErrOverflow = errors.New("filecat: kernel event buffer overflowed; events were dropped")

type Watcher struct {
	cw             *cbinding.Watcher
	root           string
	events         chan FileEvent
	errors         chan error
	coalesceWindow time.Duration

	closeOnce sync.Once
	closed    chan struct{}
	done      chan struct{}
}

// NewWatcher opens a recursive (or single-directory) watcher rooted at path
// and starts the background coalescing pipeline.
//
// bufferSize is the capacity of the Events() channel — slow consumers block
// the coalescer once it fills, so size it to your worst-case burst.
//
// coalesceWindow is the Watchman-style settle window: events arriving within
// this duration of the first event in a batch are coalesced together
// (rename halves paired into Move, repeat MODIFIED for the same path
// folded to one). 50ms is a reasonable default; smaller windows reduce
// latency at the cost of more spurious events under write storms.
//
// Call Close() when done — the watcher holds an OS handle and a goroutine
// pair that only terminate via Close.
func NewWatcher(path string, recursive bool, bufferSize int, coalesceWindow time.Duration) (*Watcher, error) {
	cw, st := cbinding.Open(path, recursive)
	if st != cbinding.StatusOK {
		return nil, fmt.Errorf("filecat: open %q: %s", path, cbinding.Strerror(st))
	}
	if bufferSize < 1 {
		bufferSize = 1
	}
	w := &Watcher{
		cw:             cw,
		root:           path,
		events:         make(chan FileEvent, bufferSize),
		errors:         make(chan error, 8),
		coalesceWindow: coalesceWindow,
		closed:         make(chan struct{}),
		done:           make(chan struct{}),
	}
	go w.run()
	return w, nil
}

// Events returns the channel of coalesced FileEvents. It is closed when the
// watcher has fully shut down — a range loop terminates naturally.
func (w *Watcher) Events() <-chan FileEvent { return w.events }

// Errors returns a channel of non-fatal errors (currently just ErrOverflow).
// The channel has a small buffer; errors are dropped if the consumer is
// slower than the producer rather than blocking the coalescer.
func (w *Watcher) Errors() <-chan error { return w.errors }

// Close stops the watcher and releases all resources. Safe to call from
// any goroutine (including an Events consumer) and idempotent across
// concurrent callers; the second caller blocks until the first completes.
func (w *Watcher) Close() error {
	w.closeOnce.Do(func() {
		w.cw.Close()
		close(w.closed)
		<-w.done
		w.cw.Destroy()
	})
	return nil
}
