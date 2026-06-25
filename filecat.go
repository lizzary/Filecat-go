// Package filecat provides cross-platform recursive directory watching.
// It is a Go wrapper over the Filecat C library, using the goroutine-per-
// watcher pattern recommended in the upstream design notes.
package filecat

import (
	"fmt"

	"github.com/lizzary/filecat-go/internal/cbinding"
)

type Watcher struct {
	w             *Watcher
	events        chan Event
	errors        chan error
	batchWindowNs int
}

type Event struct {
	Type EventType
	Path string
}

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

type FileEntry struct {
	fileId FileID
	path   string
}
type FileMap map[FileID]string

func Open(path string, recursive bool, getFileEntryById func(id FileID) FileEntry) *Watcher {

}
