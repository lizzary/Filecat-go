// Package filecat provides cross-platform recursive directory watching.
// It is a Go wrapper over the Filecat C library, using the goroutine-per-
// watcher pattern recommended in the upstream design notes.
package filecat

import (
	"context"
	"errors"
	"fmt"
	"runtime"
	"sync"

	"github.com/lizzary/filecat-go/internal/cbinding"
)

type EventType int

const (
	EventCreated    EventType = cbinding.EventCreated
	EventRemoved    EventType = cbinding.EventRemoved
	EventModified   EventType = cbinding.EventModified
	EventRenamedOld EventType = cbinding.EventRenamedOld
	EventRenamedNew EventType = cbinding.EventRenamedNew
)

func (t EventType) String() string {
	switch t {
	case EventCreated:
		return "CREATED"
	case EventRemoved:
		return "REMOVED"
	case EventModified:
		return "MODIFIED"
	case EventRenamedOld:
		return "RENAMED_OLD"
	case EventRenamedNew:
		return "RENAMED_NEW"
	}
	return fmt.Sprintf("EventType(%d)", int(t))
}

// Event is a single filesystem change.
//
// Path is relative to the watch root, in the OS's native separator form.
//
// Rename semantics differ by platform (see the upstream DESIGN.md §4):
//   - Linux / Windows: a rename is RENAMED_OLD followed immediately by
//     RENAMED_NEW. Either may appear alone if the move crosses the watch
//     boundary.
//   - macOS: both sides appear as RENAMED_OLD with best-effort (not
//     guaranteed) adjacency; the library never emits RENAMED_NEW. Treat
//     each RENAMED_OLD as "this path may have just changed identity".
type Event struct {
	Type EventType
	Path string
}

// ErrOverflow is the non-fatal error sent on the Errors channel when the
// OS event queue overflowed and one or more events were dropped. The
// watcher itself remains valid; reconcile by rescanning the tree.
var ErrOverflow = errors.New("filecat: event queue overflowed; events were dropped")

// Watcher delivers filesystem events for a directory.
//
// Construct with Open. Receive events from Events() and non-fatal errors
// from Errors(). Stop by calling Close or by cancelling the context passed
// to Open; both channels are then closed by the watcher's internal
// goroutine.
type Watcher struct {
	cw         *cbinding.Watcher
	events     chan Event
	errors     chan error
	ctx        context.Context
	cancel     context.CancelFunc
	workerDone chan struct{}
	closeOnce  sync.Once
	closeErr   error
}

// Open starts a watcher on root. If recursive is true, the entire subtree
// is watched (one OS handle on Windows/macOS, one inotify watch per
// directory on Linux).
//
// The watcher runs one internal goroutine that drains events from the C
// library and forwards them onto the Events channel. It exits when ctx is
// cancelled or Close is called.
func Open(ctx context.Context, root string, recursive bool) (*Watcher, error) {
	cw, status := cbinding.Open(root, recursive)
	if status != cbinding.StatusOK {
		return nil, fmt.Errorf("filecat: open %q: %s", root, cbinding.Strerror(status))
	}

	wctx, cancel := context.WithCancel(ctx)
	w := &Watcher{
		cw:         cw,
		events:     make(chan Event, 64),
		errors:     make(chan error, 8),
		ctx:        wctx,
		cancel:     cancel,
		workerDone: make(chan struct{}),
	}

	// Cancellation watchdog: when wctx fires (either from the user's parent
	// ctx or from Close), poke the C library so the worker's blocking
	// NextEvent returns ERR_CLOSED.
	go func() {
		<-wctx.Done()
		cw.Close()
	}()

	go w.loop()

	// Finalizer is a last-resort safety net for callers who forget Close;
	// it is not a substitute. SetFinalizer to nil in Close so the watcher
	// can be GC'd promptly after explicit Close.
	runtime.SetFinalizer(w, func(w *Watcher) { _ = w.Close() })

	return w, nil
}

// Events returns the channel events are sent on. Closed when the watcher
// stops.
func (w *Watcher) Events() <-chan Event { return w.events }

// Errors returns the channel non-fatal errors are sent on. Closed when the
// watcher stops. Currently the only value sent here is ErrOverflow.
func (w *Watcher) Errors() <-chan error { return w.errors }

// Close stops the watcher and releases its resources. Safe to call from
// any goroutine and any number of times; only the first call does work.
// Returns the fatal error that stopped the watcher, if any (nil on a
// clean shutdown via Close or context cancellation).
func (w *Watcher) Close() error {
	w.closeOnce.Do(func() {
		w.cancel()           // wakes the watchdog → C close → worker unblocks
		<-w.workerDone       // wait for the worker to drain and return
		w.cw.Destroy()       // safe now: no in-flight C calls from this side
		runtime.SetFinalizer(w, nil)
	})
	return w.closeErr
}

func (w *Watcher) loop() {
	defer close(w.workerDone)
	defer close(w.events)
	defer close(w.errors)

	for {
		typ, path, status := w.cw.NextEvent()
		switch status {
		case cbinding.StatusOK:
			select {
			case w.events <- Event{Type: EventType(typ), Path: path}:
			case <-w.ctx.Done():
				return
			}
		case cbinding.StatusErrOverflow:
			select {
			case w.errors <- ErrOverflow:
			case <-w.ctx.Done():
				return
			}
		case cbinding.StatusErrClosed:
			return
		default:
			err := fmt.Errorf("filecat: %s", cbinding.Strerror(status))
			w.closeErr = err
			select {
			case w.errors <- err:
			default:
			}
			return
		}
	}
}
