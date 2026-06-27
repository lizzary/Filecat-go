//go:build !darwin

package filecat

// resolveMove is a no-op on Linux and Windows: those backends already
// surface the rename direction unambiguously (Linux via RENAMED_OLD vs
// RENAMED_NEW from inotify, Windows via the same pair from
// ReadDirectoryChangesExW; the cross-subdir REMOVED + CREATED case
// likewise arrives in source-then-destination order).
func resolveMove(ev FileEvent) FileEvent { return ev }

// resolveHalfMove is a no-op on Linux and Windows: an unpaired half-move
// already surfaces with the correct direction. On Linux a move-out is
// IN_MOVED_FROM → RENAMED_OLD → Removed, and a move-in is IN_MOVED_TO →
// RENAMED_NEW → Created (handled in commit, never flagged fromHalf). On
// Windows the backend reports REMOVED / ADDED outright. Only macOS, where
// both rename halves arrive as RENAMED_OLD, needs the disk probe.
func resolveHalfMove(ev FileEvent) FileEvent { return ev }
