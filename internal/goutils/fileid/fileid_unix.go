//go:build linux || darwin

package fileid

import (
	"fmt"
	"os"
	"syscall"
)

func getFileID(path string) (FileID, error) {
	fi, err := os.Stat(path)
	if err != nil {
		return FileID{}, err
	}
	st, ok := fi.Sys().(*syscall.Stat_t)
	if !ok {
		return FileID{}, fmt.Errorf("goutils: unexpected FileInfo.Sys() type %T for %q", fi.Sys(), path)
	}
	return FileID{
		Volume: uint64(st.Dev),
		Index:  uint64(st.Ino),
	}, nil
}
