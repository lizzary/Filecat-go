//go:build !darwin

package filecat

// resolveMove is a no-op on Linux and Windows: those backends already
// surface the rename direction unambiguously (Linux via RENAMED_OLD vs
// RENAMED_NEW from inotify, Windows via the same pair from
// ReadDirectoryChangesExW; the cross-subdir REMOVED + CREATED case
// likewise arrives in source-then-destination order).
func resolveMove(ev FileEvent) FileEvent { return ev }
