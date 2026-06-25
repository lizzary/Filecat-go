// Package fileid Package goutils contains internal Go helpers shared across the filecat-go
// module. The public API for the module lives in the top-level filecat
// package; nothing here is part of the module's stable surface.
package fileid

import "fmt"

// FileID uniquely identifies a file or directory on the local machine.
//
// On Unix (Linux, macOS) Volume holds st_dev and Index holds st_ino from
// stat(2): the (device, inode) pair that identifies a file within a mounted
// filesystem.
//
// On Windows Volume holds VolumeSerialNumber and Index holds the 64-bit file
// index (nFileIndexHigh<<32 | nFileIndexLow) from GetFileInformationByHandle.
// For NTFS the index is the MFT record number and is stable across renames.
// On ReFS the file ID is natively 128-bit; the 64-bit form returned here may
// collide on very large volumes.
//
// Two paths refer to the same on-disk object iff their FileID values compare
// equal with ==.
type FileID struct {
	Volume uint64
	Index  uint64
}

// String renders the FileID as "volume:index" in hex.
func (id FileID) String() string {
	return fmt.Sprintf("%x:%x", id.Volume, id.Index)
}

// GetFileID returns the FileID of the file or directory at path. Symbolic
// links are followed. The returned error wraps the underlying os/syscall
// error and can be tested with errors.Is against the usual filesystem
// sentinels (fs.ErrNotExist, fs.ErrPermission, ...).
func GetFileID(path string) (FileID, error) {
	return getFileID(path)
}
