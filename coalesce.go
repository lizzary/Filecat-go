package filecat

import (
	"fmt"
	"path/filepath"
	"time"

	"github.com/lizzary/filecat-go/internal/cbinding"
)

// rawEvent is the in-flight form between the C reader and the coalescer.
// Path is kept relative here (the C library's native form); the absolute
// path is built only on the events that survive coalescing.
type rawEvent struct {
	typ           int
	path          string
	correlationID uint64
}

// run drives the watcher's two goroutines: readLoop (which blocks in the C
// ABI's filecat_next_event) and the coalescer (this goroutine).
//
// The <-rlDone join at the end is load-bearing. It guarantees readLoop has
// returned from its final cw.NextEvent before run returns — and run returning
// is what closes w.done, which is what unblocks Close's <-w.done and lets it
// call cw.Destroy. Without the join, Destroy (which frees the C watcher)
// could race a readLoop still parked inside filecat_next_event: a
// use-after-free, since the C core does not reference-count in-flight calls.
// This is exactly the close → join reader → destroy ordering the C ABI
// requires of its callers (see the C core's DESIGN.md §6.3).
func (w *Watcher) run() {
	defer close(w.done)
	defer close(w.events)

	rawCh := make(chan rawEvent, 256)
	rlDone := make(chan struct{})
	go func() {
		defer close(rlDone)
		w.readLoop(rawCh)
	}()
	w.coalesceLoop(rawCh)
	<-rlDone
}

// readLoop pulls events off the blocking C ABI and forwards them to the
// coalescer. It is the sole consumer of cw.NextEvent (the C lib requires
// single-consumer per watcher).
func (w *Watcher) readLoop(out chan<- rawEvent) {
	defer close(out)
	for {
		typ, path, id, st := w.cw.NextEvent()
		switch st {
		case cbinding.StatusOK:
			select {
			case out <- rawEvent{typ: typ, path: path, correlationID: id}:
			case <-w.closed:
				return
			}
		case cbinding.StatusErrClosed:
			return
		case cbinding.StatusErrOverflow:
			w.reportError(ErrOverflow)
		default:
			w.reportError(fmt.Errorf("filecat: %s", cbinding.Strerror(st)))
			return
		}
	}
}

func (w *Watcher) reportError(err error) {
	select {
	case w.errors <- err:
	default:
	}
}

// coalesceLoop runs the Watchman-style settle window. The timer is armed
// when the first event of a batch arrives and fires once per batch; it is
// not reset on subsequent events (so a steady stream cannot starve a
// flush).
func (w *Watcher) coalesceLoop(in <-chan rawEvent) {
	var (
		batch  *batchState
		timer  *time.Timer
		timerC <-chan time.Time
	)
	defer func() {
		if timer != nil {
			timer.Stop()
		}
	}()

	flush := func() {
		if batch == nil {
			return
		}
		for _, p := range batch.drain() {
			fe := p.fe
			fe.Path = filepath.Join(w.root, fe.Path)
			if fe.OldPath != "" {
				fe.OldPath = filepath.Join(w.root, fe.OldPath)
			}
			// A provisional Removed that came from an unpaired RENAMED_OLD
			// (macOS half-move) may actually be a move *into* the watch;
			// resolveHalfMove probes the disk to decide. Paired Moves go
			// through resolveMove for direction disambiguation. Both hooks
			// are no-ops on Linux/Windows.
			if p.fromHalf {
				fe = resolveHalfMove(fe)
			} else {
				fe = resolveMove(fe)
			}
			select {
			case w.events <- fe:
			case <-w.closed:
				return
			}
		}
		batch = nil
		timerC = nil
	}

	for {
		select {
		case ev, ok := <-in:
			if !ok {
				flush()
				return
			}
			if batch == nil {
				batch = newBatch()
				if timer == nil {
					timer = time.NewTimer(w.coalesceWindow)
				} else {
					timer.Reset(w.coalesceWindow)
				}
				timerC = timer.C
			}
			batch.absorb(ev)
		case <-timerC:
			flush()
		case <-w.closed:
			flush()
			return
		}
	}
}

// pendingFE is a committed event plus the metadata the flush step needs
// to finalize it. fromHalf marks an event that began life as an unpaired
// RENAMED_OLD: on macOS that is ambiguous between a move-out (Removed)
// and a move-in (Created), so the flusher routes it through
// resolveHalfMove instead of resolveMove.
type pendingFE struct {
	fe       FileEvent
	fromHalf bool
}

// batchState holds the in-flight coalescing state for one settle window.
//
//   - pending: pair-eligible events (CREATED / REMOVED / RENAMED_*) waiting
//     for their mate, keyed by the correlation_id the C lib surfaces.
//   - out: events committed for emission, in arrival order.
//   - lastModifiedAt: per-path index into out, used to dedup MODIFIED for
//     the same path within the window (absorbs Windows write storms).
type batchState struct {
	pending        map[uint64]rawEvent
	out            []pendingFE
	lastModifiedAt map[string]int
}

func newBatch() *batchState {
	return &batchState{
		pending:        make(map[uint64]rawEvent),
		lastModifiedAt: make(map[string]int),
	}
}

func (b *batchState) absorb(ev rawEvent) {
	// MODIFIED never participates in pairing (Windows emits multiple
	// MODIFIED per write, all sharing the file's FileId — they would
	// otherwise pair pairwise as bogus Moves).
	if ev.typ == cbinding.EventModified {
		b.recordModified(ev.path)
		return
	}
	// id == 0 (Linux create/delete) has nothing to pair on.
	if ev.correlationID == 0 {
		b.commit(ev)
		return
	}
	other, hasOther := b.pending[ev.correlationID]
	if !hasOther {
		b.pending[ev.correlationID] = ev
		return
	}
	if b.tryPair(other, ev) {
		delete(b.pending, ev.correlationID)
		return
	}
	// Same id but type combination is not a Move (e.g. CREATED then
	// REMOVED on Windows with reused FileId — that's not a rename, just
	// MFT recycling for an unrelated file). Flush the earlier one as its
	// own event and keep the newer one pending.
	b.commit(other)
	b.pending[ev.correlationID] = ev
}

// tryPair recognizes the three Move-forming combinations across platforms.
// First/second are in arrival order.
//
//	RENAMED_OLD -> RENAMED_NEW   Linux & Windows same-parent rename
//	RENAMED_OLD -> RENAMED_OLD   macOS (FSEvents emits both halves as OLD)
//	REMOVED     -> CREATED       Windows cross-subdir move; same FileId
func (b *batchState) tryPair(first, second rawEvent) bool {
	switch {
	case first.typ == cbinding.EventRenamedOld && second.typ == cbinding.EventRenamedNew,
		first.typ == cbinding.EventRenamedOld && second.typ == cbinding.EventRenamedOld,
		first.typ == cbinding.EventRemoved && second.typ == cbinding.EventCreated:
		b.out = append(b.out, pendingFE{fe: FileEvent{
			Type:    EventMove,
			OldPath: first.path,
			Path:    second.path,
		}})
		return true
	}
	return false
}

func (b *batchState) recordModified(path string) {
	if _, ok := b.lastModifiedAt[path]; ok {
		return
	}
	b.lastModifiedAt[path] = len(b.out)
	b.out = append(b.out, pendingFE{fe: FileEvent{Type: EventModified, Path: path}})
}

// commit translates a single raw event to FileEvent and appends to out.
//
// Unpaired RENAMED_NEW becomes Created (Linux half-move into watch).
// Unpaired RENAMED_OLD becomes Removed *provisionally* and is flagged
// fromHalf: on Linux/Windows that direction is final (move-out), but on
// macOS — where both halves of a rename surface as RENAMED_OLD — the
// surviving half could be a move-in, so resolveHalfMove re-checks against
// the disk at flush time. A genuine REMOVED (real delete) is never
// flagged, so it is never reclassified.
func (b *batchState) commit(ev rawEvent) {
	switch ev.typ {
	case cbinding.EventCreated, cbinding.EventRenamedNew:
		b.out = append(b.out, pendingFE{fe: FileEvent{Type: EventCreated, Path: ev.path}})
	case cbinding.EventRemoved:
		b.out = append(b.out, pendingFE{fe: FileEvent{Type: EventRemoved, Path: ev.path}})
	case cbinding.EventRenamedOld:
		b.out = append(b.out, pendingFE{fe: FileEvent{Type: EventRemoved, Path: ev.path}, fromHalf: true})
	case cbinding.EventModified:
		b.recordModified(ev.path)
	}
}

// drain finalizes the batch: pending entries that never found a mate are
// flushed as their single-event equivalent, then the ordered output slice
// is returned.
func (b *batchState) drain() []pendingFE {
	for _, ev := range b.pending {
		b.commit(ev)
	}
	b.pending = nil
	return b.out
}
