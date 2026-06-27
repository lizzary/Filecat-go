#ifndef FILECAT_H
#define FILECAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FILECAT_OK              =  0,
    FILECAT_ERR_INVALID_ARG = -1,
    FILECAT_ERR_NOT_FOUND   = -2,
    FILECAT_ERR_NO_MEMORY   = -3,
    FILECAT_ERR_OVERFLOW    = -4,  /* kernel buffer overflowed: events were dropped, watcher remains valid */
    FILECAT_ERR_SYSTEM      = -5,
    FILECAT_ERR_CLOSED      = -6
} filecat_status_t;

typedef enum {
    FILECAT_EVENT_CREATED       = 1,
    FILECAT_EVENT_REMOVED       = 2,
    FILECAT_EVENT_MODIFIED      = 3,
    FILECAT_EVENT_RENAMED_OLD   = 4,  /* old name of a rename (emitted before *_NEW) */
    FILECAT_EVENT_RENAMED_NEW   = 5
} filecat_event_type_t;

typedef struct {
    filecat_event_type_t type;
    /* UTF-8, relative to the watch root, in the OS's native separator form.
     * Owned by the watcher: valid only until the next filecat_next_event /
     * filecat_close call on the same watcher. */
    const char *path;

    /* Pairing identifier. When non-zero, events sharing the same value
     * refer to the same logical file / rename and should be correlated by
     * the consumer (key your rename-pairing hashmap on this directly).
     * Zero means the backend did not surface an id for this event.
     *
     * Linux:   inotify rename cookie. Non-zero on FILECAT_EVENT_RENAMED_OLD
     *          / FILECAT_EVENT_RENAMED_NEW only; shared by the two halves
     *          of a single rename(2). All other events are 0 — inotify
     *          does not surface inodes, and on Linux a true create/delete
     *          is never half of a move.
     * Windows: 64-bit NTFS/ReFS FileId from FILE_NOTIFY_EXTENDED_INFORMATION.
     *          Non-zero on every event for a real file; a rename's OLD/NEW
     *          pair shares it, and a delete-then-create that reuses the
     *          same MFT entry also shares it.
     * macOS:   inode from FSEvents extended data. Non-zero on every event;
     *          both halves of a rename share it (FSEvents does not pair
     *          them itself — the consumer pairs on this id). */
    uint64_t event_correlation_id;
} filecat_event_t;

/* Non-zero if this event carries a pairing id the consumer should correlate
 * with other events sharing the same id. Defined as (ev->event_correlation_id
 * != 0); exposed as a function purely for self-documenting call sites. */
static inline int filecat_event_pairable(const filecat_event_t *ev) {
    return ev->event_correlation_id != 0;
}

typedef struct filecat_watcher filecat_watcher_t;

/* Open a directory watcher.
 *
 *   path:      UTF-8 path to an existing directory.
 *   recursive: 0 -> watch only `path` (one level);
 *              non-zero -> watch the whole subtree (maps to Windows
 *              bWatchSubtree / equivalent on other platforms).
 *   out:       on success, receives the new watcher. Release with
 *              filecat_destroy; optionally cancel a blocking
 *              filecat_next_event from another thread with filecat_close
 *              first.
 *
 * Returns FILECAT_OK on success.
 */
filecat_status_t filecat_open(const char *path, int recursive, filecat_watcher_t **out);

/* Block until the next event is available, then fill `out`.
 *
 * Returns FILECAT_ERR_CLOSED once another thread has called filecat_close.
 *
 * On FILECAT_ERR_OVERFLOW the watcher remains valid; the caller may keep
 * calling filecat_next_event (subsequent events arrive normally; the lost ones
 * are not recoverable).
 *
 * Threading: at most one thread may call filecat_next_event on a given watcher
 * at a time (single consumer). It IS safe to call filecat_close concurrently
 * from another thread to cancel a blocking call -- it will then return
 * FILECAT_ERR_CLOSED. It is NOT safe to race with filecat_destroy: the
 * consumer must have returned from its final filecat_next_event before any
 * thread calls filecat_destroy (see filecat_destroy).
 */
filecat_status_t filecat_next_event(filecat_watcher_t *w, filecat_event_t *out);

/* Signal the watcher to stop. Thread-safe and idempotent: any thread may call
 * this any number of times. A pending filecat_next_event call returns
 * FILECAT_ERR_CLOSED. Does NOT free the watcher -- pair with filecat_destroy.
 * Safe to call with NULL.
 */
void filecat_close(filecat_watcher_t *w);

/* Release the watcher's OS resources and free it.
 *
 * There is NO reference counting: destroy frees immediately. The caller MUST
 * guarantee, before calling filecat_destroy:
 *   1. the consumer has observed FILECAT_ERR_CLOSED from filecat_next_event
 *      and will not call filecat_next_event or filecat_close on this watcher
 *      again. Call filecat_close first to make a blocking consumer return
 *      FILECAT_ERR_CLOSED, then join/queiesce that consumer; and
 *   2. exactly one thread calls filecat_destroy, exactly once.
 *
 * Calling any filecat_* function on the watcher after destroy has been
 * entered -- including a second filecat_destroy -- is undefined behavior;
 * the memory is already gone. (This is a deliberate simplification over a
 * refcounted close: the binding above this library owns lifetime and is in a
 * far better position to join its consumer than the C core is to second-guess
 * it.)
 *
 * The typical pattern: the owner calls filecat_close, joins the consumer
 * thread, then calls filecat_destroy exactly once.
 *
 * Safe to call with NULL.
 */
void filecat_destroy(filecat_watcher_t *w);

/* Human-readable description of a status code. Returned string is static. */
const char *filecat_strerror(filecat_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* FILECAT_H */
