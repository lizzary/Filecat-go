package filecat

import (
	"github.com/lizzary/filecat-go/internal/goutils/fileid"
)

// FileID is a platform-stable identifier for a file. On Unix it is the
// (device, inode) pair; on Windows it is (volume serial, file index).
// Two paths share a FileID iff they refer to the same underlying file.
type FileID fileid.FileID

// String returns a stable, human-readable form of the identifier.
func (id FileID) String() string {
	return fileid.FileID(id).String()
}

// GetFileID resolves the FileID of the file at path. It returns an error
// if the path cannot be stat-ed.
func GetFileID(path string) (FileID, error) {
	id, err := fileid.GetFileID(path)
	return FileID(id), err
}
