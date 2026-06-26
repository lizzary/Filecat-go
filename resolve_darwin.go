//go:build darwin

package filecat

import (
	"errors"
	"io/fs"
	"os"
)

// resolveMove disambiguates rename direction on macOS by stat'ing both
// sides of a Move event.
//
// FSEvents reports BOTH halves of a rename as RENAMED_OLD with no
// direction hint, so the coalescer pairs by inode and uses arrival order
// as the initial guess (first = OldPath, second = Path). By the time the
// settle window flushes the rename has almost always settled to disk:
// the source path no longer exists and the destination does. If the disk
// disagrees with the arrival-order guess we swap.
//
// If both paths exist, neither exists, or stat fails for some other
// reason (permission, network FS), the initial guess is preserved — we
// only swap on unambiguous evidence.
func resolveMove(ev FileEvent) FileEvent {
	if ev.Type != EventMove || ev.OldPath == "" || ev.Path == "" {
		return ev
	}
	oldExists := pathExists(ev.OldPath)
	newExists := pathExists(ev.Path)
	if oldExists && !newExists {
		ev.OldPath, ev.Path = ev.Path, ev.OldPath
	}
	return ev
}

func pathExists(p string) bool {
	if _, err := os.Lstat(p); err == nil {
		return true
	} else if errors.Is(err, fs.ErrNotExist) {
		return false
	}
	// Stat failed for a non-"not exist" reason (EACCES, network FS, etc.).
	// Treat as "exists" so we do not swap on inconclusive evidence — the
	// arrival-order guess stays.
	return true
}
