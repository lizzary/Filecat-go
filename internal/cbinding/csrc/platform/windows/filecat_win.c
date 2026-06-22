#include "filecat/filecat.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* 64 KB: also the documented upper bound for ReadDirectoryChangesW on network shares. */
#define FILECAT_BUFFER_SIZE (64u * 1024u)

#define FILECAT_NOTIFY_FILTER  \
    ( FILE_NOTIFY_CHANGE_FILE_NAME   \
    | FILE_NOTIFY_CHANGE_DIR_NAME    \
    | FILE_NOTIFY_CHANGE_ATTRIBUTES  \
    | FILE_NOTIFY_CHANGE_SIZE        \
    | FILE_NOTIFY_CHANGE_LAST_WRITE )

struct filecat_watcher {
    HANDLE  hDir;
    BOOL    bWatchSubtree;
    BYTE   *buffer;                  /* FILECAT_BUFFER_SIZE bytes, DWORD-aligned */
    FILE_NOTIFY_INFORMATION *current; /* next record to emit, or NULL if buffer drained */
    char   *utf8_path;                /* scratch UTF-8 buffer returned via event.path */
    int     utf8_capacity;

    /* Lifecycle bookkeeping. All accessed via Interlocked*; reads use the
     * no-op CAS idiom InterlockedCompareExchange(&x, 0, 0). */
    volatile LONG refcount;   /* 1 owner ref + 1 per in-flight call */
    volatile LONG closing;    /* set-once latch (0 -> 1) */
    volatile LONG destroyed;  /* set-once latch (0 -> 1); guards owner-ref drop */
};

static filecat_status_t map_win_error(DWORD err)
{
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return FILECAT_ERR_NOT_FOUND;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return FILECAT_ERR_NO_MEMORY;
        case ERROR_NOTIFY_ENUM_DIR:
            return FILECAT_ERR_OVERFLOW;
        case ERROR_OPERATION_ABORTED:
        case ERROR_INVALID_HANDLE:
            return FILECAT_ERR_CLOSED;
        case ERROR_INVALID_PARAMETER:
            return FILECAT_ERR_INVALID_ARG;
        default:
            return FILECAT_ERR_SYSTEM;
    }
}

static filecat_event_type_t map_action(DWORD action)
{
    switch (action) {
        case FILE_ACTION_ADDED:            return FILECAT_EVENT_CREATED;
        case FILE_ACTION_REMOVED:          return FILECAT_EVENT_REMOVED;
        case FILE_ACTION_MODIFIED:         return FILECAT_EVENT_MODIFIED;
        case FILE_ACTION_RENAMED_OLD_NAME: return FILECAT_EVENT_RENAMED_OLD;
        case FILE_ACTION_RENAMED_NEW_NAME: return FILECAT_EVENT_RENAMED_NEW;
        default:                           return FILECAT_EVENT_MODIFIED;
    }
}

static wchar_t *utf8_to_wide(const char *utf8)
{
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, wlen) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

/* Convert a UTF-8 path to a wide path that bypasses MAX_PATH.
 *
 * Resolves relative paths, normalizes separators and . / .., and prepends
 * the \\?\ prefix (or \\?\UNC\ for UNC paths). Paths the caller has already
 * prefixed with \\?\ or \\.\ are returned unchanged.
 */
static wchar_t *utf8_to_wide_path(const char *utf8)
{
    wchar_t *raw = utf8_to_wide(utf8);
    if (!raw) return NULL;

    /* Already prefixed -- trust the caller. */
    if (raw[0] == L'\\' && raw[1] == L'\\' &&
        (raw[2] == L'?' || raw[2] == L'.') && raw[3] == L'\\') {
        return raw;
    }

    /* GetFullPathNameW(W version) accepts long inputs and produces long outputs;
     * it also turns / into \ and resolves . / .. for us. */
    DWORD need = GetFullPathNameW(raw, 0, NULL, NULL);
    if (need == 0) { free(raw); return NULL; }
    wchar_t *full = (wchar_t *)malloc(sizeof(wchar_t) * need);
    if (!full) { free(raw); return NULL; }
    DWORD got = GetFullPathNameW(raw, need, full, NULL);
    free(raw);
    if (got == 0 || got >= need) { free(full); return NULL; }

    /* UNC \\server\share\foo  -> \\?\UNC\server\share\foo
     * Drive C:\foo            -> \\?\C:\foo                 */
    int is_unc = (full[0] == L'\\' && full[1] == L'\\');
    const wchar_t *prefix     = is_unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    size_t         prefix_len = is_unc ? 8u              : 4u;
    const wchar_t *body       = is_unc ? full + 2        : full; /* skip leading "\\" for UNC */
    size_t         body_len   = is_unc ? (size_t)got - 2 : (size_t)got;

    wchar_t *out = (wchar_t *)malloc(sizeof(wchar_t) * (prefix_len + body_len + 1));
    if (!out) { free(full); return NULL; }
    memcpy(out, prefix, prefix_len * sizeof(wchar_t));
    memcpy(out + prefix_len, body, body_len * sizeof(wchar_t));
    out[prefix_len + body_len] = L'\0';
    free(full);
    return out;
}

/* Convert a (non-null-terminated) wide string of `wlen_chars` WCHARs into
 * w->utf8_path, growing the buffer as needed. */
static filecat_status_t store_event_path(filecat_watcher_t *w,
                                         const WCHAR *wname, int wlen_chars)
{
    int needed = 0;
    if (wlen_chars > 0) {
        needed = WideCharToMultiByte(CP_UTF8, 0, wname, wlen_chars,
                                     NULL, 0, NULL, NULL);
        if (needed <= 0) return FILECAT_ERR_SYSTEM;
    }

    if (needed + 1 > w->utf8_capacity) {
        int new_cap = needed + 1;
        char *p = (char *)realloc(w->utf8_path, (size_t)new_cap);
        if (!p) return FILECAT_ERR_NO_MEMORY;
        w->utf8_path = p;
        w->utf8_capacity = new_cap;
    }

    if (needed > 0) {
        int got = WideCharToMultiByte(CP_UTF8, 0, wname, wlen_chars,
                                      w->utf8_path, w->utf8_capacity, NULL, NULL);
        if (got <= 0) return FILECAT_ERR_SYSTEM;
        w->utf8_path[got] = '\0';
    } else {
        w->utf8_path[0] = '\0';
    }
    return FILECAT_OK;
}

/* ---- refcount + close latches --------------------------------------------
 *
 * Lifetime model:
 *   - filecat_open initializes refcount=1 (the owner ref).
 *   - filecat_next_event / filecat_close retain at entry, release at exit.
 *   - filecat_destroy CAS-sets the `destroyed` latch (so only one call drops
 *     the owner ref) and then releases.
 *   - The last release does the real free.
 *
 * `closing` is a set-once latch separate from `destroyed`: filecat_close flips
 * `closing` (without touching `destroyed`), filecat_destroy flips both. The
 * actual CloseHandle is gated by `closing` so it runs exactly once.
 */

static void watcher_free(filecat_watcher_t *w)
{
    /* Refcount has just dropped to 0. Paranoid: if no close ever happened,
     * close the handle now. */
    HANDLE h = (HANDLE)InterlockedExchangePointer((PVOID volatile *)&w->hDir, NULL);
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(w->buffer);
    free(w->utf8_path);
    free(w);
}

static void watcher_retain(filecat_watcher_t *w)
{
    InterlockedIncrement(&w->refcount);
}

static void watcher_release(filecat_watcher_t *w)
{
    if (InterlockedDecrement(&w->refcount) == 0) {
        watcher_free(w);
    }
}

/* Flip the `closing` latch and close the directory handle exactly once.
 * Idempotent and thread-safe.
 *
 * CancelIoEx is required to wake a consumer thread that is parked inside
 * a synchronous ReadDirectoryChangesW: CloseHandle alone does NOT cancel
 * an in-flight sync I/O on a directory handle — the kernel keeps the
 * underlying file object alive until the I/O completes, and a directory-
 * change wait completes only when an event arrives. CancelIoEx (Vista+,
 * NULL OVERLAPPED targets every pending operation on the handle) aborts
 * the wait so RDCW returns ERROR_OPERATION_ABORTED, which our caller
 * maps to FILECAT_ERR_CLOSED. */
static void watcher_close_handle(filecat_watcher_t *w)
{
    if (InterlockedExchange(&w->closing, 1) == 0) {
        HANDLE h = (HANDLE)InterlockedExchangePointer((PVOID volatile *)&w->hDir, NULL);
        if (h && h != INVALID_HANDLE_VALUE) {
            CancelIoEx(h, NULL);
            CloseHandle(h);
        }
    }
}

filecat_status_t filecat_open(const char *path, int recursive, filecat_watcher_t **out)
{
    if (!path || !out) return FILECAT_ERR_INVALID_ARG;
    *out = NULL;

    wchar_t *wpath = utf8_to_wide_path(path);
    if (!wpath) return FILECAT_ERR_INVALID_ARG;

    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        free(wpath);
        return map_win_error(err);
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        free(wpath);
        return FILECAT_ERR_INVALID_ARG;
    }

    HANDLE h = CreateFileW(
        wpath,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,   /* required for directory handles */
        NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        return map_win_error(GetLastError());
    }

    filecat_watcher_t *w = (filecat_watcher_t *)calloc(1, sizeof(*w));
    if (!w) {
        CloseHandle(h);
        return FILECAT_ERR_NO_MEMORY;
    }
    /* malloc on Windows returns at least 8-byte aligned memory, satisfying the
     * DWORD alignment requirement of FILE_NOTIFY_INFORMATION. */
    w->buffer = (BYTE *)malloc(FILECAT_BUFFER_SIZE);
    if (!w->buffer) {
        free(w);
        CloseHandle(h);
        return FILECAT_ERR_NO_MEMORY;
    }
    w->hDir          = h;
    w->bWatchSubtree = recursive ? TRUE : FALSE;
    /* current, utf8_path, utf8_capacity, closing, destroyed already 0 from calloc */
    w->refcount      = 1;  /* owner ref, released by filecat_destroy */

    *out = w;
    return FILECAT_OK;
}

filecat_status_t filecat_next_event(filecat_watcher_t *w, filecat_event_t *out)
{
    if (!w || !out) return FILECAT_ERR_INVALID_ARG;
    watcher_retain(w);

    /* Fast path: another thread already requested close. Skip the kernel
     * call entirely and exit. */
    if (InterlockedCompareExchange(&w->closing, 0, 0) != 0) {
        watcher_release(w);
        return FILECAT_ERR_CLOSED;
    }

    /* Drain any leftover records from the previous read first. */
    if (w->current == NULL) {
        /* Snapshot the handle once: close may NULL it out from another
         * thread; the kernel call will then fail with ERROR_INVALID_HANDLE
         * which we map to FILECAT_ERR_CLOSED. */
        HANDLE h = w->hDir;
        if (h == NULL || h == INVALID_HANDLE_VALUE) {
            watcher_release(w);
            return FILECAT_ERR_CLOSED;
        }
        DWORD bytes = 0;
        BOOL ok = ReadDirectoryChangesW(
            h, w->buffer, FILECAT_BUFFER_SIZE,
            w->bWatchSubtree, FILECAT_NOTIFY_FILTER,
            &bytes,
            NULL,    /* synchronous: no OVERLAPPED */
            NULL);   /* no completion routine */
        if (!ok) {
            DWORD err = GetLastError();
            watcher_release(w);
            return map_win_error(err);
        }
        if (bytes == 0) {
            /* Documented overflow signal for synchronous calls. The kernel
             * buffer was drained; subsequent reads will receive new events. */
            watcher_release(w);
            return FILECAT_ERR_OVERFLOW;
        }
        w->current = (FILE_NOTIFY_INFORMATION *)w->buffer;
    }

    FILE_NOTIFY_INFORMATION *fni = w->current;
    out->type = map_action(fni->Action);

    int wlen_chars = (int)(fni->FileNameLength / sizeof(WCHAR));
    filecat_status_t s = store_event_path(w, fni->FileName, wlen_chars);
    if (s != FILECAT_OK) {
        watcher_release(w);
        return s;
    }
    out->path = w->utf8_path;

    /* Advance to next record in this buffer, or mark the buffer drained. */
    if (fni->NextEntryOffset == 0) {
        w->current = NULL;
    } else {
        w->current = (FILE_NOTIFY_INFORMATION *)((BYTE *)fni + fni->NextEntryOffset);
    }
    watcher_release(w);
    return FILECAT_OK;
}

void filecat_close(filecat_watcher_t *w)
{
    if (!w) return;
    watcher_retain(w);
    watcher_close_handle(w);
    watcher_release(w);
}

void filecat_destroy(filecat_watcher_t *w)
{
    if (!w) return;
    /* CAS-once: only the first destroy actually drops the owner ref.
     * Concurrent destroy calls on a still-valid pointer are safe. */
    if (InterlockedExchange(&w->destroyed, 1) != 0) return;
    /* Ensure the handle is closed even if filecat_close was never called. */
    watcher_close_handle(w);
    /* Drop the owner ref. Frees w once the last in-flight call releases. */
    watcher_release(w);
}

const char *filecat_strerror(filecat_status_t status)
{
    switch (status) {
        case FILECAT_OK:               return "ok";
        case FILECAT_ERR_INVALID_ARG:  return "invalid argument";
        case FILECAT_ERR_NOT_FOUND:    return "path not found";
        case FILECAT_ERR_NO_MEMORY:    return "out of memory";
        case FILECAT_ERR_OVERFLOW:     return "kernel buffer overflow: events were dropped";
        case FILECAT_ERR_SYSTEM:       return "system error";
        case FILECAT_ERR_CLOSED:       return "watcher closed";
        default:                       return "unknown error";
    }
}
