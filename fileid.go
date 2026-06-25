package filecat

import (
	"github.com/lizzary/filecat-go/internal/goutils/fileid"
)

type FileID fileid.FileID

func (id FileID) String() string {
	return fileid.FileID(id).String()
}

func GetFileID(path string) (FileID, error) {
	id, err := fileid.GetFileID(path)
	return FileID(id), err
}
