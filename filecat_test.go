package filecat_test

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/lizzary/filecat-go"
)

// =============================================================================
//  Test infrastructure
// =============================================================================

const (
	testCoalesceWindow = 50 * time.Millisecond
	testBufferSize     = 4096
	settleAfterStart   = 150 * time.Millisecond
)

// settleAfterOps is how long to wait after an FS operation before draining
// events. FSEvents on macOS is markedly slower than inotify / RDCW, so we
// give it a longer window — matching the C test suite's TH_SETTLE_MS knob.
var settleAfterOps = func() time.Duration {
	if runtime.GOOS == "darwin" {
		return 800 * time.Millisecond
	}
	return 300 * time.Millisecond
}()

// newWatcher creates an empty temp dir, runs setup (if non-nil) before the
// watcher is opened so setup events do not leak into the test, then opens a
// recursive (or non-recursive) watcher and waits for it to be ready. The
// watcher is closed automatically at test end.
//
// The returned dir has had filepath.EvalSymlinks applied so that path
// comparisons survive platforms where t.TempDir() returns a symlinked path
// (notably macOS /tmp → /private/tmp). The watcher is opened against the
// canonical path so emitted events use it too.
func newWatcher(t *testing.T, recursive bool, setup func(dir string)) (string, *filecat.Watcher) {
	t.Helper()
	raw := t.TempDir()
	dir, err := filepath.EvalSymlinks(raw)
	if err != nil {
		dir = raw
	}
	if setup != nil {
		setup(dir)
	}
	w, err := filecat.NewWatcher(dir, recursive, testBufferSize, testCoalesceWindow)
	if err != nil {
		t.Fatalf("NewWatcher(%q): %v", dir, err)
	}
	t.Cleanup(func() { _ = w.Close() })
	// Brief settle so the C reader goroutine has actually entered
	// filecat_next_event before the test mutates the FS. On Windows
	// ReadDirectoryChangesExW only captures events delivered while a call
	// is pending; anything that happens between Open and the first
	// NextEvent can be dropped. Linux/macOS buffer pre-call events, but
	// the delay is cheap and uniform.
	time.Sleep(settleAfterStart)
	return dir, w
}

// drain reads from the events channel for d, returning all events received.
// Returns early if the channel is closed.
func drain(t *testing.T, w *filecat.Watcher, d time.Duration) []filecat.FileEvent {
	t.Helper()
	var out []filecat.FileEvent
	deadline := time.NewTimer(d)
	defer deadline.Stop()
	for {
		select {
		case ev, ok := <-w.Events():
			if !ok {
				return out
			}
			out = append(out, ev)
		case <-deadline.C:
			return out
		}
	}
}

func eventsString(es []filecat.FileEvent) string {
	parts := make([]string, 0, len(es))
	for _, e := range es {
		if e.Type == filecat.EventMove {
			parts = append(parts, fmt.Sprintf("%s(%s -> %s)", e.Type, e.OldPath, e.Path))
		} else {
			parts = append(parts, fmt.Sprintf("%s(%s)", e.Type, e.Path))
		}
	}
	return "[" + strings.Join(parts, ", ") + "]"
}

func contains(es []filecat.FileEvent, typ filecat.EventType, path string) bool {
	for _, e := range es {
		if e.Type == typ && e.Path == path {
			return true
		}
	}
	return false
}

func containsMove(es []filecat.FileEvent, oldPath, newPath string) bool {
	for _, e := range es {
		if e.Type == filecat.EventMove && e.OldPath == oldPath && e.Path == newPath {
			return true
		}
	}
	return false
}

func countType(es []filecat.FileEvent, typ filecat.EventType) int {
	n := 0
	for _, e := range es {
		if e.Type == typ {
			n++
		}
	}
	return n
}

func mustWrite(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
}

func mustMkdir(t *testing.T, path string) {
	t.Helper()
	if err := os.Mkdir(path, 0o755); err != nil {
		t.Fatalf("mkdir %s: %v", path, err)
	}
}

func mustRename(t *testing.T, src, dst string) {
	t.Helper()
	if err := os.Rename(src, dst); err != nil {
		t.Fatalf("rename %s -> %s: %v", src, dst, err)
	}
}

// =============================================================================
//  Lifecycle and API surface
// =============================================================================

func TestNewWatcher_OpenAndClose(t *testing.T) {
	dir := t.TempDir()
	w, err := filecat.NewWatcher(dir, true, 64, testCoalesceWindow)
	if err != nil {
		t.Fatalf("NewWatcher: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	// Events must close so a range loop terminates naturally.
	if _, ok := <-w.Events(); ok {
		t.Error("Events channel should be closed after Close()")
	}
}

func TestClose_Idempotent(t *testing.T) {
	dir := t.TempDir()
	w, err := filecat.NewWatcher(dir, true, 64, testCoalesceWindow)
	if err != nil {
		t.Fatal(err)
	}
	// Multiple closes from the same goroutine must not panic or block.
	for i := 0; i < 3; i++ {
		if err := w.Close(); err != nil {
			t.Errorf("Close #%d: %v", i, err)
		}
	}
}

func TestClose_FromMultipleGoroutines(t *testing.T) {
	dir := t.TempDir()
	w, err := filecat.NewWatcher(dir, true, 64, testCoalesceWindow)
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_ = w.Close()
		}()
	}
	done := make(chan struct{})
	go func() { wg.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("concurrent Close deadlocked")
	}
}

func TestClose_UnblocksPendingConsumer(t *testing.T) {
	dir := t.TempDir()
	w, err := filecat.NewWatcher(dir, true, 64, testCoalesceWindow)
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		for range w.Events() {
		}
	}()
	time.Sleep(50 * time.Millisecond)
	_ = w.Close()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("consumer did not unblock after Close()")
	}
}

func TestNewWatcher_NonExistentPath(t *testing.T) {
	bogus := filepath.Join(t.TempDir(), "does", "not", "exist")
	_, err := filecat.NewWatcher(bogus, true, 64, testCoalesceWindow)
	if err == nil {
		t.Fatal("expected error opening nonexistent path")
	}
}

func TestEventType_String(t *testing.T) {
	cases := map[filecat.EventType]string{
		filecat.EventCreated:  "Created",
		filecat.EventRemoved:  "Removed",
		filecat.EventModified: "Modified",
		filecat.EventMove:     "Move",
	}
	for typ, want := range cases {
		if got := typ.String(); got != want {
			t.Errorf("EventType(%d).String() = %q, want %q", int(typ), got, want)
		}
	}
}

// =============================================================================
//  Correctness — full user-action matrix from README
//
//  Numbering matches the README table (① through ⑮). Each test reproduces
//  one user-visible action and asserts the FileEvent the binding emits.
// =============================================================================

// ① Create file
func TestAction_CreateFile(t *testing.T) {
	dir, w := newWatcher(t, true, nil)
	target := filepath.Join(dir, "new.txt")
	mustWrite(t, target, nil)
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventCreated, target) {
		t.Fatalf("missing Created(%s)\nevents: %s", target, eventsString(events))
	}
}

// ② Write file content — the per-write event must arrive (post-create it
//
//	becomes a Modified; the dedup behavior under bursts is tested below).
func TestAction_ModifyFile(t *testing.T) {
	target := ""
	dir, w := newWatcher(t, true, func(d string) {
		target = filepath.Join(d, "x.txt")
		mustWrite(t, target, []byte("v0"))
	})
	_ = dir
	mustWrite(t, target, []byte("v1"))
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventModified, target) {
		t.Fatalf("missing Modified(%s)\nevents: %s", target, eventsString(events))
	}
}

// ③ Change attributes
func TestAction_ChmodFile(t *testing.T) {
	target := ""
	dir, w := newWatcher(t, true, func(d string) {
		target = filepath.Join(d, "perms.txt")
		mustWrite(t, target, []byte("data"))
	})
	_ = dir
	// On Windows os.Chmod toggles the read-only bit, which fires
	// FILE_ACTION_MODIFIED (attrs). On Linux/macOS chmod fires
	// IN_ATTRIB / ItemInodeMetaMod. All three become Modified.
	if err := os.Chmod(target, 0o444); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(target, 0o644) })
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventModified, target) {
		t.Fatalf("missing Modified(%s) after chmod\nevents: %s", target, eventsString(events))
	}
}

// ④ Delete file
func TestAction_DeleteFile(t *testing.T) {
	target := ""
	dir, w := newWatcher(t, true, func(d string) {
		target = filepath.Join(d, "gone.txt")
		mustWrite(t, target, nil)
	})
	_ = dir
	if err := os.Remove(target); err != nil {
		t.Fatal(err)
	}
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventRemoved, target) {
		t.Fatalf("missing Removed(%s)\nevents: %s", target, eventsString(events))
	}
}

// ⑤ Rename file, same parent — should collapse to a single Move.
func TestAction_RenameFile_SameParent(t *testing.T) {
	src := ""
	dir, w := newWatcher(t, true, func(d string) {
		src = filepath.Join(d, "a.txt")
		mustWrite(t, src, []byte("data"))
	})
	dst := filepath.Join(dir, "b.txt")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !containsMove(events, src, dst) {
		t.Fatalf("missing Move(%s -> %s)\nevents: %s", src, dst, eventsString(events))
	}
	// Must not also leak both halves as separate Created/Removed.
	if contains(events, filecat.EventCreated, dst) {
		t.Errorf("unexpected Created(%s) alongside Move\nevents: %s", dst, eventsString(events))
	}
	if contains(events, filecat.EventRemoved, src) {
		t.Errorf("unexpected Removed(%s) alongside Move\nevents: %s", src, eventsString(events))
	}
}

// ⑥ Move file across watched subdirs — should collapse to a Move
//
//	despite the OS reporting it as REMOVED + CREATED on Windows.
func TestAction_MoveFile_CrossSubdir(t *testing.T) {
	src := ""
	dir, w := newWatcher(t, true, func(d string) {
		mustMkdir(t, filepath.Join(d, "a"))
		mustMkdir(t, filepath.Join(d, "b"))
		src = filepath.Join(d, "a", "x.txt")
		mustWrite(t, src, []byte("data"))
	})
	dst := filepath.Join(dir, "b", "x.txt")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !containsMove(events, src, dst) {
		t.Fatalf("missing Move(%s -> %s)\nevents: %s", src, dst, eventsString(events))
	}
}

// ⑦ Move file OUT of watch — surfaces as Removed (the destination is
//
//	outside the watched subtree, so no second half ever arrives).
func TestAction_MoveFile_OutOfWatch(t *testing.T) {
	src := ""
	dir, w := newWatcher(t, true, func(d string) {
		src = filepath.Join(d, "leaving.txt")
		mustWrite(t, src, []byte("data"))
	})
	_ = dir
	outside, err := os.MkdirTemp("", "filecat-test-out-*")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(outside) })
	if outsideReal, err := filepath.EvalSymlinks(outside); err == nil {
		outside = outsideReal
	}
	dst := filepath.Join(outside, "leaving.txt")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventRemoved, src) {
		t.Fatalf("missing Removed(%s) for move-out\nevents: %s", src, eventsString(events))
	}
	if countType(events, filecat.EventMove) != 0 {
		t.Errorf("unexpected Move event for half-move\nevents: %s", eventsString(events))
	}
}

// ⑧ Move file INTO watch.
//
//	Linux: IN_MOVED_TO without a mate → RENAMED_NEW → Created.
//	Windows: ADDED → Created.
//	macOS: ItemRenamed (destination side) → RENAMED_OLD. This is an
//	  UNPAIRED single event, indistinguishable at the event layer from
//	  a half-move-OUT (⑦). commit types it provisionally as Removed and
//	  flags it fromHalf; at flush time resolveHalfMove (resolve_darwin.go)
//	  disk-probes the path — it exists (the move landed), so the event is
//	  rewritten to Created. The residual cost is a settle-window race: a
//	  move-in immediately followed by a delete can still misclassify,
//	  since the probe only sees the final on-disk state.
func TestAction_MoveFile_IntoWatch(t *testing.T) {
	dir, w := newWatcher(t, true, nil)
	outside, err := os.MkdirTemp("", "filecat-test-in-*")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(outside) })
	if outsideReal, err := filepath.EvalSymlinks(outside); err == nil {
		outside = outsideReal
	}
	src := filepath.Join(outside, "arriving.txt")
	mustWrite(t, src, []byte("data"))
	dst := filepath.Join(dir, "arriving.txt")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventCreated, dst) {
		t.Fatalf("missing Created(%s) for move-in\nevents: %s", dst, eventsString(events))
	}
}

// ⑨ Atomic replace — mv a b where b already exists. The binding should
//
//	still emit a single Move (the kernel surfaces it as a rename pair on
//	every platform).
func TestAction_AtomicReplace(t *testing.T) {
	src, dst := "", ""
	dir, w := newWatcher(t, true, func(d string) {
		src = filepath.Join(d, "a.txt")
		dst = filepath.Join(d, "b.txt")
		mustWrite(t, src, []byte("a"))
		mustWrite(t, dst, []byte("b"))
	})
	_ = dir
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !containsMove(events, src, dst) {
		t.Fatalf("missing Move(%s -> %s) for atomic replace\nevents: %s",
			src, dst, eventsString(events))
	}
}

// ⑩ Create directory
func TestAction_CreateDirectory(t *testing.T) {
	dir, w := newWatcher(t, true, nil)
	target := filepath.Join(dir, "newdir")
	mustMkdir(t, target)
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventCreated, target) {
		t.Fatalf("missing Created(%s)\nevents: %s", target, eventsString(events))
	}
}

// ⑪ Delete directory
func TestAction_DeleteDirectory(t *testing.T) {
	target := ""
	dir, w := newWatcher(t, true, func(d string) {
		target = filepath.Join(d, "doomed")
		mustMkdir(t, target)
	})
	_ = dir
	if err := os.Remove(target); err != nil {
		t.Fatal(err)
	}
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventRemoved, target) {
		t.Fatalf("missing Removed(%s)\nevents: %s", target, eventsString(events))
	}
}

// ⑫ Rename directory, same parent
func TestAction_RenameDirectory_SameParent(t *testing.T) {
	src := ""
	dir, w := newWatcher(t, true, func(d string) {
		src = filepath.Join(d, "olddir")
		mustMkdir(t, src)
	})
	dst := filepath.Join(dir, "newdir")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !containsMove(events, src, dst) {
		t.Fatalf("missing Move(%s -> %s)\nevents: %s", src, dst, eventsString(events))
	}
}

// ⑬ Move directory across watched subdirs
func TestAction_MoveDirectory_CrossSubdir(t *testing.T) {
	src := ""
	dir, w := newWatcher(t, true, func(d string) {
		mustMkdir(t, filepath.Join(d, "a"))
		mustMkdir(t, filepath.Join(d, "b"))
		src = filepath.Join(d, "a", "sub")
		mustMkdir(t, src)
	})
	dst := filepath.Join(dir, "b", "sub")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !containsMove(events, src, dst) {
		t.Fatalf("missing Move(%s -> %s)\nevents: %s", src, dst, eventsString(events))
	}
}

// ⑭ Directory moved OUT of watch
func TestAction_MoveDirectory_OutOfWatch(t *testing.T) {
	src := ""
	dir, w := newWatcher(t, true, func(d string) {
		src = filepath.Join(d, "leaving")
		mustMkdir(t, src)
	})
	_ = dir
	outside, err := os.MkdirTemp("", "filecat-test-dirout-*")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(outside) })
	if outsideReal, err := filepath.EvalSymlinks(outside); err == nil {
		outside = outsideReal
	}
	dst := filepath.Join(outside, "leaving")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventRemoved, src) {
		t.Fatalf("missing Removed(%s) for dir move-out\nevents: %s", src, eventsString(events))
	}
}

// ⑮ Directory moved INTO watch — same macOS half-move handling as the
//
//	file case (resolveHalfMove disk-probes the unpaired RENAMED_OLD and
//	rewrites it to Created); see TestAction_MoveFile_IntoWatch.
func TestAction_MoveDirectory_IntoWatch(t *testing.T) {
	dir, w := newWatcher(t, true, nil)
	outside, err := os.MkdirTemp("", "filecat-test-dirin-*")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(outside) })
	if outsideReal, err := filepath.EvalSymlinks(outside); err == nil {
		outside = outsideReal
	}
	src := filepath.Join(outside, "arriving")
	mustMkdir(t, src)
	dst := filepath.Join(dir, "arriving")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventCreated, dst) {
		t.Fatalf("missing Created(%s) for dir move-in\nevents: %s", dst, eventsString(events))
	}
}

// =============================================================================
//  Coalescer-specific behavior
// =============================================================================

// TestCoalesce_ModifyDedup verifies that a burst of writes to one file
// within a single settle window collapses to a single Modified event.
// This is the Watchman-style behavior that absorbs Windows write storms
// (one fwrite → size + last-write + attrs MODIFIED events).
func TestCoalesce_ModifyDedup(t *testing.T) {
	raw := t.TempDir()
	dir, err := filepath.EvalSymlinks(raw)
	if err != nil {
		dir = raw
	}
	target := filepath.Join(dir, "burst.txt")
	mustWrite(t, target, []byte("v0"))

	// A 250 ms window comfortably brackets a 20-iteration burst even on
	// noisy CI runners — every write should fall into one batch.
	w, err := filecat.NewWatcher(dir, true, testBufferSize, 250*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = w.Close() })
	time.Sleep(settleAfterStart)

	for i := 0; i < 20; i++ {
		mustWrite(t, target, []byte(fmt.Sprintf("v%d", i)))
	}
	events := drain(t, w, 700*time.Millisecond)
	mods := countType(events, filecat.EventModified)
	if mods == 0 {
		t.Fatalf("no Modified events captured\nevents: %s", eventsString(events))
	}
	// One batch should be one event; allow at most 2 for cross-window
	// boundary noise on slow machines.
	if mods > 2 {
		t.Errorf("modify dedup failed: got %d Modified events from 20 writes\nevents: %s",
			mods, eventsString(events))
	}
}

// TestCoalesce_RenamePairCollapses verifies that the two halves of a
// rename arriving within one settle window emit one Move, not two raw
// events.
func TestCoalesce_RenamePairCollapses(t *testing.T) {
	src := ""
	dir, w := newWatcher(t, true, func(d string) {
		src = filepath.Join(d, "a.txt")
		mustWrite(t, src, []byte("data"))
	})
	dst := filepath.Join(dir, "b.txt")
	mustRename(t, src, dst)
	events := drain(t, w, settleAfterOps)
	moves := countType(events, filecat.EventMove)
	if moves != 1 {
		t.Errorf("expected exactly 1 Move event, got %d\nevents: %s", moves, eventsString(events))
	}
	if countType(events, filecat.EventCreated) > 0 || countType(events, filecat.EventRemoved) > 0 {
		t.Errorf("rename halves leaked as Created/Removed\nevents: %s", eventsString(events))
	}
}

// TestNonRecursive verifies recursive=false: events inside subdirs must
// not propagate, but events in the watch root itself must.
func TestNonRecursive(t *testing.T) {
	dir, w := newWatcher(t, false, func(d string) {
		mustMkdir(t, filepath.Join(d, "sub"))
	})
	insideSub := filepath.Join(dir, "sub", "ignored.txt")
	atRoot := filepath.Join(dir, "noticed.txt")
	mustWrite(t, insideSub, nil)
	mustWrite(t, atRoot, nil)
	events := drain(t, w, settleAfterOps)
	if !contains(events, filecat.EventCreated, atRoot) {
		t.Errorf("missing Created(%s) at watch root\nevents: %s", atRoot, eventsString(events))
	}
	if contains(events, filecat.EventCreated, insideSub) {
		t.Errorf("non-recursive watcher leaked subdir event Created(%s)\nevents: %s",
			insideSub, eventsString(events))
	}
}

// TestRoundTripRename — A → B → A within one window should net to nothing
// observable on most platforms; on others both halves can pair into a
// single Move(A → A) round-trip. Either way, the consumer must not see
// the file disappear.
func TestRoundTripRename(t *testing.T) {
	a := ""
	dir, w := newWatcher(t, true, func(d string) {
		a = filepath.Join(d, "a.txt")
		mustWrite(t, a, []byte("data"))
	})
	b := filepath.Join(dir, "b.txt")
	mustRename(t, a, b)
	mustRename(t, b, a)
	events := drain(t, w, settleAfterOps)
	// Whatever the binding emits, the file at A must not appear "gone"
	// to a consumer who only sees the final state — i.e. there should
	// be no Removed(A) without a matching Created(A) or Move(*→A).
	removed := 0
	restored := 0
	for _, e := range events {
		switch e.Type {
		case filecat.EventRemoved:
			if e.Path == a {
				removed++
			}
		case filecat.EventCreated:
			if e.Path == a {
				restored++
			}
		case filecat.EventMove:
			if e.Path == a {
				restored++
			}
		}
	}
	if removed > restored {
		t.Errorf("round-trip rename leaked a net Removed(%s): removed=%d restored=%d\nevents: %s",
			a, removed, restored, eventsString(events))
	}
}

// =============================================================================
//  FileID public API
// =============================================================================

func TestFileID_StableAndUnique(t *testing.T) {
	dir := t.TempDir()
	a := filepath.Join(dir, "a.txt")
	b := filepath.Join(dir, "b.txt")
	mustWrite(t, a, []byte("a"))
	mustWrite(t, b, []byte("b"))

	idA, err := filecat.GetFileID(a)
	if err != nil {
		t.Fatalf("GetFileID(a): %v", err)
	}
	idA2, err := filecat.GetFileID(a)
	if err != nil {
		t.Fatalf("GetFileID(a) again: %v", err)
	}
	idB, err := filecat.GetFileID(b)
	if err != nil {
		t.Fatalf("GetFileID(b): %v", err)
	}
	if idA != idA2 {
		t.Errorf("same file → different IDs: %s vs %s", idA, idA2)
	}
	if idA == idB {
		t.Errorf("different files → same ID: %s == %s", idA, idB)
	}
	if idA.String() == "" || !strings.Contains(idA.String(), ":") {
		t.Errorf("FileID.String() malformed: %q", idA.String())
	}
}

func TestFileID_NonExistent(t *testing.T) {
	_, err := filecat.GetFileID(filepath.Join(t.TempDir(), "nope"))
	if err == nil {
		t.Fatal("expected error for nonexistent path")
	}
	if !errors.Is(err, fs.ErrNotExist) {
		t.Errorf("expected fs.ErrNotExist, got %v", err)
	}
}

// =============================================================================
//  Stress
// =============================================================================

// TestStress_ManyCreates verifies the binding survives a large burst of
// distinct file creates and that the event count is roughly proportional
// to the operation count (allowing some loss to FSEvents coalescing on
// macOS, which is documented in the matrix).
func TestStress_ManyCreates(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping stress test in -short mode")
	}
	dir, w := newWatcher(t, true, nil)
	const N = 1000

	for i := 0; i < N; i++ {
		mustWrite(t, filepath.Join(dir, fmt.Sprintf("f%05d.txt", i)), nil)
	}

	// Drain for longer than usual — the burst takes some time to surface.
	events := drain(t, w, 5*time.Second)
	created := countType(events, filecat.EventCreated)

	// Linux/Windows surface every CREATED individually; macOS FSEvents
	// may coalesce. Accept >= 80% on macOS, full count elsewhere.
	want := N
	if runtime.GOOS == "darwin" {
		want = N * 4 / 5
	}
	if created < want {
		t.Errorf("got %d Created events out of %d operations (want >= %d)\nfirst events: %s",
			created, N, want, truncEvents(events, 10))
	}
	t.Logf("captured %d Created events from %d ops (total events: %d)", created, N, len(events))
}

// TestStress_ConcurrentProducers runs four goroutines hammering the watch
// directory with creates / writes / deletes, and verifies the binding
// drains events without crashing, deadlocking, or returning errors other
// than the well-defined ErrOverflow.
func TestStress_ConcurrentProducers(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping stress test in -short mode")
	}
	dir, w := newWatcher(t, true, nil)

	var totalEvents atomic.Int64
	consumerDone := make(chan struct{})
	go func() {
		defer close(consumerDone)
		for range w.Events() {
			totalEvents.Add(1)
		}
	}()

	const (
		producers   = 4
		opsPerProd  = 250
		burstWrites = 3
	)
	var (
		opsRun atomic.Int64
		wg     sync.WaitGroup
	)
	for g := 0; g < producers; g++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for i := 0; i < opsPerProd; i++ {
				p := filepath.Join(dir, fmt.Sprintf("g%d-f%d", id, i))
				for w := 0; w < burstWrites; w++ {
					if err := os.WriteFile(p, []byte("x"), 0o644); err != nil {
						return
					}
				}
				switch i % 3 {
				case 0:
					_ = os.Remove(p)
				case 1:
					_ = os.Rename(p, p+".moved")
				}
				opsRun.Add(1)
			}
		}(g)
	}
	wg.Wait()

	// Allow the coalescer to drain the tail.
	time.Sleep(settleAfterOps * 3)
	_ = w.Close()

	select {
	case <-consumerDone:
	case <-time.After(5 * time.Second):
		t.Fatal("consumer goroutine did not exit after Close()")
	}

	// Non-blocking drain of any error reports.
	overflows := 0
	other := 0
draining:
	for {
		select {
		case err, ok := <-w.Errors():
			if !ok {
				break draining
			}
			if errors.Is(err, filecat.ErrOverflow) {
				overflows++
			} else {
				other++
				t.Logf("non-overflow error: %v", err)
			}
		default:
			break draining
		}
	}

	if other > 0 {
		t.Errorf("got %d non-overflow errors during stress", other)
	}
	if totalEvents.Load() == 0 {
		t.Error("no events collected during concurrent stress")
	}
	t.Logf("ran %d ops across %d producers; captured %d events, %d overflows",
		opsRun.Load(), producers, totalEvents.Load(), overflows)
}

// TestStress_ManyDirectories sets up a wide tree (one watcher rooted at
// the top), then creates files across all of its leaf directories. This
// exercises the recursive watching path — on Linux it forces the C core
// to install per-directory inotify watches; on Win/macOS it confirms the
// single subtree handle/stream actually covers descendants.
func TestStress_ManyDirectories(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping stress test in -short mode")
	}
	const dirs = 50
	dir, w := newWatcher(t, true, func(d string) {
		for i := 0; i < dirs; i++ {
			mustMkdir(t, filepath.Join(d, fmt.Sprintf("d%02d", i)))
		}
	})

	for i := 0; i < dirs; i++ {
		for j := 0; j < 5; j++ {
			p := filepath.Join(dir, fmt.Sprintf("d%02d", i), fmt.Sprintf("f%d.txt", j))
			mustWrite(t, p, nil)
		}
	}

	events := drain(t, w, 3*time.Second)
	created := countType(events, filecat.EventCreated)
	want := dirs * 5
	if runtime.GOOS == "darwin" {
		want = want * 4 / 5
	}
	if created < want {
		t.Errorf("recursive watch missed events: got %d Created across %d dirs, want >= %d\nfirst: %s",
			created, dirs, want, truncEvents(events, 10))
	}
}

// TestStress_RapidOpenClose verifies a tight Open/Close loop does not
// leak goroutines or OS handles and — critically — does not race the C
// watcher's teardown. This is the regression test for the close/destroy
// use-after-free: Close calls cw.Destroy(), which frees the C watcher,
// and an earlier build did so while readLoop could still be parked inside
// the C filecat_next_event call. run() now joins readLoop before letting
// Close proceed to Destroy (see coalesce.go), so 100 rapid cycles complete
// cleanly instead of corrupting the heap. It used to abort with
// STATUS_HEAP_CORRUPTION (0xc0000374) on Windows.
func TestStress_RapidOpenClose(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping stress test in -short mode")
	}
	dir := t.TempDir()
	for i := 0; i < 100; i++ {
		w, err := filecat.NewWatcher(dir, true, 64, testCoalesceWindow)
		if err != nil {
			t.Fatalf("open #%d: %v", i, err)
		}
		if err := w.Close(); err != nil {
			t.Fatalf("close #%d: %v", i, err)
		}
	}
}

func truncEvents(es []filecat.FileEvent, n int) string {
	if len(es) <= n {
		return eventsString(es)
	}
	return eventsString(es[:n]) + fmt.Sprintf(" ... (+%d more)", len(es)-n)
}
