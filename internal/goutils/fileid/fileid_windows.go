//go:build windows

package fileid

import "syscall"

// FILE_READ_ATTRIBUTES is the minimum access right needed to query metadata
// via GetFileInformationByHandle without opening the file for data access.
// Not exposed by the stdlib syscall package, so we redeclare it here.
const fileReadAttributes = 0x80

func getFileID(path string) (FileID, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return FileID{}, err
	}
	// FILE_FLAG_BACKUP_SEMANTICS is required to obtain a handle to a
	// directory; harmless on regular files. Symlinks/reparse points are
	// followed by default, matching os.Stat on Unix.
	h, err := syscall.CreateFile(
		p,
		fileReadAttributes,
		syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE|syscall.FILE_SHARE_DELETE,
		nil,
		syscall.OPEN_EXISTING,
		syscall.FILE_FLAG_BACKUP_SEMANTICS,
		0,
	)
	if err != nil {
		return FileID{}, err
	}
	defer syscall.CloseHandle(h)

	var info syscall.ByHandleFileInformation
	if err := syscall.GetFileInformationByHandle(h, &info); err != nil {
		return FileID{}, err
	}
	return FileID{
		Volume: uint64(info.VolumeSerialNumber),
		Index:  uint64(info.FileIndexHigh)<<32 | uint64(info.FileIndexLow),
	}, nil
}
