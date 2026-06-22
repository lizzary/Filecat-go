#ifndef FILECAT_H
#define FILECAT_H

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
} filecat_event_t;

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
 * Returns FILECAT_ERR_CLOSED once another thread has called filecat_close
 * (or filecat_destroy without prior close).
 *
 * On FILECAT_ERR_OVERFLOW the watcher remains valid; the caller may keep
 * calling filecat_next_event (subsequent events arrive normally; the lost ones
 * are not recoverable).
 *
 * Not safe to call concurrently with itself on the same watcher (only one
 * consumer at a time). It IS safe to race with filecat_close /
 * filecat_destroy from other threads -- the call will then return
 * FILECAT_ERR_CLOSED.
 */
filecat_status_t filecat_next_event(filecat_watcher_t *w, filecat_event_t *out);

/* Signal the watcher to stop. Thread-safe and idempotent: any thread may call
 * this any number of times. A pending filecat_next_event call returns
 * FILECAT_ERR_CLOSED. Does NOT free the watcher -- pair with filecat_destroy.
 * Safe to call with NULL.
 */
void filecat_close(filecat_watcher_t *w);

/* Release the watcher. Internally calls filecat_close first if needed, then
 * drops the owner reference. The library reference-counts in-flight
 * filecat_next_event / filecat_close calls, so memory is freed only after
 * the last such call returns -- a destroy concurrent with another thread's
 * blocking next_event will not cause a use-after-free.
 *
 * Idempotent across racing concurrent calls (CAS-once internally). But the
 * caller MUST NOT use the watcher pointer (or pass it to any filecat_*
 * function) after the FIRST filecat_destroy on it has returned, since the
 * memory may already be freed by then. The typical pattern is: one owner
 * thread calls destroy exactly once after all other use is done; multiple
 * threads coordinating with sync.Once (Go) or equivalent is also fine.
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
