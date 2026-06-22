// Package cbinding is a thin cgo wrapper around the upstream Filecat C ABI.
// It is private to this module; the public API lives in the top-level
// filecat package.
package cbinding

/*
#cgo CFLAGS: -I${SRCDIR}/csrc/include
#cgo darwin LDFLAGS: -framework CoreServices

#include <stdlib.h>
#include "filecat/filecat.h"
*/
import "C"

import "unsafe"

const (
	StatusOK            = 0
	StatusErrInvalidArg = -1
	StatusErrNotFound   = -2
	StatusErrNoMemory   = -3
	StatusErrOverflow   = -4
	StatusErrSystem     = -5
	StatusErrClosed     = -6
)

const (
	EventCreated    = 1
	EventRemoved    = 2
	EventModified   = 3
	EventRenamedOld = 4
	EventRenamedNew = 5
)

type Watcher struct {
	handle *C.filecat_watcher_t
}

func Open(path string, recursive bool) (*Watcher, int) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))

	var rec C.int
	if recursive {
		rec = 1
	}

	var h *C.filecat_watcher_t
	s := C.filecat_open(cpath, rec, &h)
	if s != C.FILECAT_OK {
		return nil, int(s)
	}
	return &Watcher{handle: h}, 0
}

// NextEvent blocks until the next event, the watcher is closed, or an error
// occurs. The returned path string is copied out of the C-owned buffer here,
// so the caller does not have to worry about the upstream "valid only until
// the next call" lifetime contract.
func (w *Watcher) NextEvent() (eventType int, path string, status int) {
	var ev C.filecat_event_t
	s := C.filecat_next_event(w.handle, &ev)
	if s != C.FILECAT_OK {
		return 0, "", int(s)
	}
	return int(ev._type), C.GoString(ev.path), 0
}

// Close is thread-safe and idempotent; it unblocks any in-flight NextEvent
// on this watcher with StatusErrClosed.
func (w *Watcher) Close() {
	if w != nil && w.handle != nil {
		C.filecat_close(w.handle)
	}
}

// Destroy releases the watcher. The caller MUST ensure no goroutine will
// touch the Watcher after Destroy returns. The C library reference-counts
// in-flight calls internally, so a Destroy racing with another goroutine's
// NextEvent / Close is safe, but using the Watcher pointer after Destroy is
// not — its memory may already be freed.
func (w *Watcher) Destroy() {
	if w != nil && w.handle != nil {
		C.filecat_destroy(w.handle)
	}
}

func Strerror(status int) string {
	return C.GoString(C.filecat_strerror(C.filecat_status_t(status)))
}
