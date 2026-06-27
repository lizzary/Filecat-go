#include "filecat/filecat.h"

/* Pin the SDK feature gate so ReadDirectoryChangesExW and
 * FILE_NOTIFY_EXTENDED_INFORMATION are visible. Both were introduced in
 * Windows 10 1709 (build 16299, NTDDI_WIN10_RS3). We target Win10/11
 * exclusively -- no fallback to the legacy ReadDirectoryChangesW. */
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00          /* Windows 10 */
#endif
#ifndef NTDDI_VERSION
#  define NTDDI_VERSION 0x0A000004     /* NTDDI_WIN10_RS3 (1709) */
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
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
    BYTE   *buffer;                            /* FILECAT_BUFFER_SIZE bytes, DWORD-aligned */
    FILE_NOTIFY_EXTENDED_INFORMATION *current; /* next record to emit, or NULL if buffer drained */
    char   *utf8_path;                         /* scratch UTF-8 buffer returned via event.path */
    int     utf8_capacity;

    /* close ∥ next_event handshake. Set-once latch (0 -> 1) flipped by
     * filecat_close from any thread to cancel a blocking next_event. Written
     * with InterlockedExchange, read with InterlockedCompareExchange (an
     * atomic acquire-load) so the cancel is observed even in the bare-window
     * case (consumer between two reads, where CancelIoEx has no pending I/O
     * to abort) and on weakly-ordered archs, not just x86/x64. No refcount /
     * destroyed latch: the watcher's memory lifetime is owned by the caller
     * per the filecat_destroy contract (the consumer must have exited first). */
    volatile LONG closing;
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

/* ---- lifecycle model (caller-managed, no refcount) -----------------------
 *
 *   - filecat_open returns a watcher owned by the caller.
 *   - filecat_close may be called from any thread, any number of times, to
 *     cancel a blocking filecat_next_event. It flips `closing` and issues
 *     CancelIoEx; it does NOT close the directory handle.
 *   - filecat_destroy closes the handle and frees the watcher. Per the ABI
 *     contract the caller guarantees the consumer has already observed
 *     FILECAT_ERR_CLOSED and will not touch the watcher again, and that
 *     exactly one thread calls destroy — so no in-flight call can race the
 *     free and no reference counting is needed.
 *
 * Why CancelIoEx: a consumer parked inside synchronous ReadDirectoryChangesExW
 * is not woken by anything short of an event or an explicit cancel. CancelIoEx
 * (NULL OVERLAPPED targets every pending op on the handle) aborts the wait so
 * RDCW returns ERROR_OPERATION_ABORTED, mapped to FILECAT_ERR_CLOSED. The
 * handle deliberately stays open until destroy: a close landing between two
 * reads would otherwise invalidate the handle the consumer is about to reuse.
 * The `closing` latch covers that gap — the consumer checks it before each
 * read, so a close that finds no pending I/O to cancel is still observed. */

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
    /* current, utf8_path, utf8_capacity, closing already 0 from calloc */

    *out = w;
    return FILECAT_OK;
}

filecat_status_t filecat_next_event(filecat_watcher_t *w, filecat_event_t *out)
{
    if (!w || !out) return FILECAT_ERR_INVALID_ARG;

    /* close ∥ next_event: another thread may flip `closing` to cancel us.
     * Check before entering the kernel — CancelIoEx only aborts an I/O that
     * is already pending, so a close that lands while we're between reads
     * (draining the buffer, or back in the caller) would otherwise be missed
     * until the next real event arrives. InterlockedCompareExchange(_,0,0) is
     * an atomic acquire-load that pairs with the InterlockedExchange in
     * filecat_close. */
    if (InterlockedCompareExchange(&w->closing, 0, 0) != 0)
        return FILECAT_ERR_CLOSED;

    /* Drain any leftover records from the previous read first. */
    if (w->current == NULL) {
        HANDLE h = w->hDir;
        DWORD bytes = 0;
        /* ReadDirectoryChangesExW with ReadDirectoryNotifyExtendedInformation
         * fills the buffer with FILE_NOTIFY_EXTENDED_INFORMATION records,
         * which carry the NTFS FileId (and ParentFileId, timestamps, size,
         * attrs) alongside the action and name. Win10 1709+ / Win11. A
         * concurrent filecat_close calls CancelIoEx on this handle, which
         * makes the call fail with ERROR_OPERATION_ABORTED -> FILECAT_ERR_CLOSED. */
        BOOL ok = ReadDirectoryChangesExW(
            h, w->buffer, FILECAT_BUFFER_SIZE,
            w->bWatchSubtree, FILECAT_NOTIFY_FILTER,
            &bytes,
            NULL,    /* synchronous: no OVERLAPPED */
            NULL,    /* no completion routine */
            ReadDirectoryNotifyExtendedInformation);
        if (!ok) {
            return map_win_error(GetLastError());
        }
        if (bytes == 0) {
            /* Documented overflow signal for synchronous calls. The kernel
             * buffer was drained; subsequent reads will receive new events. */
            return FILECAT_ERR_OVERFLOW;
        }
        w->current = (FILE_NOTIFY_EXTENDED_INFORMATION *)w->buffer;
    }

    FILE_NOTIFY_EXTENDED_INFORMATION *fni = w->current;
    out->type = map_action(fni->Action);

    int wlen_chars = (int)(fni->FileNameLength / sizeof(WCHAR));
    filecat_status_t s = store_event_path(w, fni->FileName, wlen_chars);
    if (s != FILECAT_OK) {
        return s;
    }
    out->path = w->utf8_path;
    /* FileId is the 64-bit NTFS/ReFS file reference. A rename's OLD/NEW
     * halves share it; so does a delete-then-create that reuses the MFT
     * entry. Downstream keys its pairing hashmap on this. */
    out->event_correlation_id = (uint64_t)fni->FileId.QuadPart;

    /* Advance to next record in this buffer, or mark the buffer drained. */
    if (fni->NextEntryOffset == 0) {
        w->current = NULL;
    } else {
        w->current = (FILE_NOTIFY_EXTENDED_INFORMATION *)((BYTE *)fni + fni->NextEntryOffset);
    }
    return FILECAT_OK;
}

void filecat_close(filecat_watcher_t *w)
{
    if (!w) return;
    /* Wake a blocking next_event. Idempotent and safe from any thread: both
     * InterlockedExchange and an extra CancelIoEx are harmless if repeated.
     * The handle is NOT closed here — that happens in filecat_destroy, after
     * the consumer has exited — so a close racing a next_event between reads
     * cannot pull the handle out from under it. */
    InterlockedExchange(&w->closing, 1);
    HANDLE h = w->hDir;
    if (h && h != INVALID_HANDLE_VALUE) CancelIoEx(h, NULL);
}

void filecat_destroy(filecat_watcher_t *w)
{
    if (!w) return;
    /* Contract: the consumer has already observed FILECAT_ERR_CLOSED and will
     * not call any filecat_* on `w` again; exactly one thread calls destroy.
     * So we can close the handle and free without any reference counting. */
    if (w->hDir && w->hDir != INVALID_HANDLE_VALUE) {
        CloseHandle(w->hDir);
        w->hDir = NULL;
    }
    free(w->buffer);
    free(w->utf8_path);
    free(w);
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
