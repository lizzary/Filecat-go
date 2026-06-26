/* Filecat — macOS backend (FSEvents v2 + extended data + dispatch queue).
 *
 * The lifecycle model (refcount + two set-once latches) is the same as the
 * Windows and Linux backends; only the I/O substrate differs. FSEvents
 * pushes batches of events into a callback running on a serial dispatch
 * queue; the callback strips the watch root, classifies each event, reads
 * the inode out of kFSEventStreamEventExtendedFileIDKey, and appends nodes
 * to a linked-list queue. filecat_next_event blocks on a pthread_cond_t.
 *
 * Requires macOS 10.13+ for kFSEventStreamCreateFlagUseExtendedData.
 *
 * Rename note: FSEvents sets kFSEventStreamEventFlagItemRenamed on both
 * sides of a rename without pairing them, but the inode (extended data)
 * is identical on both sides — downstream pairs via
 * event.event_correlation_id. As on Linux, every renamed item is surfaced
 * as FILECAT_EVENT_RENAMED_OLD; the library does not synthesize OLD/NEW
 * from native flags.
 */

/* Force dispatch types to be plain C handles (not Objective-C objects under
 * ARC) so dispatch_release / dispatch_retain are real function calls in
 * this translation unit. Must precede any Apple system header; the #undef
 * defends against `-DOS_OBJECT_USE_OBJC=1` on the command line. */
#ifdef OS_OBJECT_USE_OBJC
#undef OS_OBJECT_USE_OBJC
#endif
#define OS_OBJECT_USE_OBJC 0

#include "filecat/filecat.h"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- per-event node in the producer→consumer linked-list queue --------- */
struct fc_node {
    filecat_event_type_t type;
    uint64_t             inode;      /* 0 if extended data didn't provide one */
    char                *rel_path;   /* malloc'd UTF-8 relative path, owned */
    struct fc_node      *next;
};

struct filecat_watcher {
    /* configuration */
    char  *root;        /* canonical absolute path from realpath() */
    size_t root_len;    /* strlen(root); used to strip prefix      */
    int    recursive;   /* 0 -> drop events deeper than one level  */

    /* FSEvents stream and the serial queue its callback runs on */
    FSEventStreamRef stream;
    dispatch_queue_t queue;

    /* producer/consumer queue: FSEvents callback appends, next_event pops */
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    struct fc_node *head;
    struct fc_node *tail;
    int             overflow_pending;   /* sticky; consumed by next_event */

    /* scratch buffer aliased by event.path until the next call/close */
    char  *utf8_path;
    size_t utf8_capacity;

    /* lifecycle (mirror of Windows / Linux backends):
     *   refcount  = 1 owner ref + 1 per in-flight call; last drop frees.
     *   closing   = set-once latch; gates FSEvents teardown exactly once.
     *   destroyed = set-once latch; gates owner-ref drop exactly once. */
    atomic_int refcount;
    atomic_int closing;
    atomic_int destroyed;
};

/* ---- error mapping ----------------------------------------------------- */

static filecat_status_t map_errno(int e)
{
    switch (e) {
        case ENOENT:
        case ENOTDIR:
            return FILECAT_ERR_NOT_FOUND;
        case ENOMEM:
            return FILECAT_ERR_NO_MEMORY;
        case EINVAL:
            return FILECAT_ERR_INVALID_ARG;
        default:
            return FILECAT_ERR_SYSTEM;
    }
}

/* ---- FSEvents flag → filecat event type --------------------------------
 *
 * FSEvents flags are a bitmask: a single batch entry may set several at
 * once (e.g. Created|Modified when a file is created and written before
 * the batch is flushed). We collapse to ONE event by priority below.
 */
static filecat_event_type_t map_flags(FSEventStreamEventFlags f)
{
    if (f & kFSEventStreamEventFlagItemRenamed) {
        /* FSEvents reports the same flag on both sides of a rename and
         * never tells us which is which. By design we don't infer; both
         * halves are surfaced as RENAMED_OLD and the consumer pairs them
         * via event_correlation_id (the shared inode). */
        return FILECAT_EVENT_RENAMED_OLD;
    }
    if (f & kFSEventStreamEventFlagItemRemoved) return FILECAT_EVENT_REMOVED;
    if (f & ( kFSEventStreamEventFlagItemModified
            | kFSEventStreamEventFlagItemInodeMetaMod
            | kFSEventStreamEventFlagItemXattrMod
            | kFSEventStreamEventFlagItemFinderInfoMod
            | kFSEventStreamEventFlagItemChangeOwner))
        return FILECAT_EVENT_MODIFIED;
    if (f & kFSEventStreamEventFlagItemCreated) return FILECAT_EVENT_CREATED;

    return FILECAT_EVENT_MODIFIED;
}

/* ---- relative-path computation ---------------------------------------- *
 *
 * FSEvents hands back absolute, canonical UTF-8 paths under (or equal to)
 * the watch root. Returns 1 (accept) on success and fills *rel/*rel_len
 * with a pointer into `abs` plus its length, or 0 if the event should be
 * dropped (root itself, outside root, or below one level when !recursive).
 */
static int compute_rel(const filecat_watcher_t *w,
                       const char *abs, size_t abs_len,
                       const char **rel_out, size_t *rel_len_out)
{
    /* Trim trailing '/' that FSEvents occasionally appends to directory
     * paths; otherwise the non-recursive check below would reject a
     * legitimate "dir/" as if it had an internal separator. */
    while (abs_len > w->root_len && abs[abs_len - 1] == '/') abs_len--;

    if (abs_len < w->root_len) return 0;
    if (memcmp(abs, w->root, w->root_len) != 0) return 0;

    /* Exact match = the watch root itself; we never emit those (matches
     * Windows ReadDirectoryChangesW, which doesn't report the dir handle's
     * own changes). */
    if (abs_len == w->root_len) return 0;

    const char *rel;
    if (w->root_len == 1 && w->root[0] == '/') {
        /* root "/": no separator to skip after the prefix */
        rel = abs + 1;
    } else {
        if (abs[w->root_len] != '/') return 0;   /* prefix-with-no-sep, e.g. "/foo" vs "/foobar" */
        rel = abs + w->root_len + 1;
    }
    size_t rel_len = (size_t)((abs + abs_len) - rel);

    if (!w->recursive) {
        /* recursive=0: accept only events whose parent IS the root —
         * i.e. no '/' inside the relative path. */
        for (size_t k = 0; k < rel_len; k++) {
            if (rel[k] == '/') return 0;
        }
    }
    *rel_out     = rel;
    *rel_len_out = rel_len;
    return 1;
}

/* ---- FSEvents callback (runs on the serial dispatch queue) ------------ *
 *
 * With kFSEventStreamCreateFlagUseExtendedData set, `eventPaths` is a
 * CFArrayRef of CFDictionaryRef. Each dict carries:
 *   kFSEventStreamEventExtendedDataPathKey -> CFString (the absolute path)
 *   kFSEventStreamEventExtendedFileIDKey   -> CFNumber (the inode, uint64)
 *
 * We build a local linked-list of nodes BEFORE touching the shared mutex,
 * then splice it in at the end — keeps the consumer's wake-up critical
 * section short, and we never call malloc while holding the mutex.
 */
static void fsevents_cb(ConstFSEventStreamRef stream,
                        void *info,
                        size_t numEvents,
                        void *eventPaths,
                        const FSEventStreamEventFlags eventFlags[],
                        const FSEventStreamEventId   eventIds[])
{
    (void)stream; (void)eventIds;
    filecat_watcher_t *w = (filecat_watcher_t *)info;
    CFArrayRef events = (CFArrayRef)eventPaths;

    struct fc_node *local_head = NULL, *local_tail = NULL;
    int local_overflow = 0;

    for (size_t i = 0; i < numEvents; i++) {
        FSEventStreamEventFlags fl = eventFlags[i];

        /* Drop/scan flags → OVERFLOW. We still process the rest of the
         * batch; the consumer will see one OVERFLOW followed by the
         * remaining events, matching the Linux/Windows recovery semantics. */
        if (fl & ( kFSEventStreamEventFlagMustScanSubDirs
                 | kFSEventStreamEventFlagUserDropped
                 | kFSEventStreamEventFlagKernelDropped)) {
            local_overflow = 1;
        }

        CFDictionaryRef dict =
            (CFDictionaryRef)CFArrayGetValueAtIndex(events, (CFIndex)i);
        if (!dict) continue;

        CFStringRef cf_path = (CFStringRef)CFDictionaryGetValue(
            dict, kFSEventStreamEventExtendedDataPathKey);
        if (!cf_path) continue;

        /* CFStringGetFileSystemRepresentation produces the canonical
         * UTF-8 form macOS uses on disk (NFD). Size bound includes the
         * trailing NUL. */
        CFIndex max_len = CFStringGetMaximumSizeOfFileSystemRepresentation(cf_path);
        char *abs = (char *)malloc((size_t)max_len);
        if (!abs) continue;
        if (!CFStringGetFileSystemRepresentation(cf_path, abs, max_len)) {
            free(abs);
            continue;
        }
        size_t abs_len = strlen(abs);

        const char *rel; size_t rel_len;
        if (!compute_rel(w, abs, abs_len, &rel, &rel_len)) {
            free(abs);
            continue;
        }

        char *rp = (char *)malloc(rel_len + 1);
        if (!rp) { free(abs); continue; }
        memcpy(rp, rel, rel_len);
        rp[rel_len] = '\0';
        free(abs);

        /* Inode from extended data. CFNumber doesn't have an unsigned
         * 64-bit type; SInt64 with reinterpreted bits is the convention,
         * and real inodes fit comfortably below 2^63. */
        uint64_t inode = 0;
        CFNumberRef cf_inode = (CFNumberRef)CFDictionaryGetValue(
            dict, kFSEventStreamEventExtendedFileIDKey);
        if (cf_inode) {
            CFNumberGetValue(cf_inode, kCFNumberSInt64Type, &inode);
        }

        struct fc_node *node = (struct fc_node *)malloc(sizeof(*node));
        if (!node) { free(rp); continue; }
        node->type     = map_flags(fl);
        node->inode    = inode;
        node->rel_path = rp;
        node->next     = NULL;

        if (local_tail) local_tail->next = node;
        else            local_head       = node;
        local_tail = node;
    }

    pthread_mutex_lock(&w->mu);
    if (atomic_load_explicit(&w->closing, memory_order_acquire)) {
        /* Watcher is being torn down; drop the local list. */
        pthread_mutex_unlock(&w->mu);
        while (local_head) {
            struct fc_node *n = local_head;
            local_head = n->next;
            free(n->rel_path);
            free(n);
        }
        return;
    }
    if (local_head) {
        if (w->tail) w->tail->next = local_head;
        else         w->head       = local_head;
        w->tail = local_tail;
    }
    if (local_overflow) w->overflow_pending = 1;
    if (local_head || local_overflow) pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

/* ---- refcount + close latches ----------------------------------------- *
 *
 * Lifetime model (verbatim mirror of the Windows backend):
 *   - filecat_open initializes refcount=1 (the owner ref).
 *   - filecat_next_event / filecat_close retain at entry, release at exit.
 *   - filecat_destroy CAS-sets `destroyed` (so only one call drops the
 *     owner ref) and then releases.
 *   - The last release does the real free.
 */

static void watcher_free(filecat_watcher_t *w)
{
    /* The stream has been torn down inside watcher_close_internal by the
     * time we get here; the queue is still owned by us. */
    if (w->queue) dispatch_release(w->queue);

    /* Drain any events that were enqueued but never consumed. */
    struct fc_node *n = w->head;
    while (n) {
        struct fc_node *next = n->next;
        free(n->rel_path);
        free(n);
        n = next;
    }

    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    free(w->root);
    free(w->utf8_path);
    free(w);
}

static void watcher_retain(filecat_watcher_t *w)
{
    atomic_fetch_add_explicit(&w->refcount, 1, memory_order_relaxed);
}

static void watcher_release(filecat_watcher_t *w)
{
    if (atomic_fetch_sub_explicit(&w->refcount, 1, memory_order_acq_rel) == 1)
        watcher_free(w);
}

/* No-op used by dispatch_sync_f to "fence" the serial queue: when it
 * runs, every previously-submitted callback has finished. */
static void noop_dispatch_fn(void *ctx) { (void)ctx; }

static void watcher_close_internal(filecat_watcher_t *w)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&w->closing, &expected, 1)) return;

    /* Tear down the FSEvents stream. After Invalidate no new callback will
     * be delivered for this stream. Release drops our +1 ref. */
    if (w->stream) {
        FSEventStreamStop(w->stream);
        FSEventStreamInvalidate(w->stream);
        FSEventStreamRelease(w->stream);
        w->stream = NULL;
    }

    /* Drain any callback that was already in flight on the serial queue.
     * Once this returns, no callback can still be executing against `w`. */
    if (w->queue) {
        dispatch_sync_f(w->queue, NULL, noop_dispatch_fn);
    }

    /* Wake any consumer parked in pthread_cond_wait. The broadcast is done
     * under the mutex so the standard `while (predicate) cond_wait` idiom
     * in filecat_next_event can't miss the signal. */
    pthread_mutex_lock(&w->mu);
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

/* ---- public API ------------------------------------------------------- */

filecat_status_t filecat_open(const char *path, int recursive,
                              filecat_watcher_t **out)
{
    if (!path || !out) return FILECAT_ERR_INVALID_ARG;
    *out = NULL;

    char *root = realpath(path, NULL);
    if (!root) return map_errno(errno);

    struct stat st;
    if (stat(root, &st) != 0) {
        int e = errno;
        free(root);
        return map_errno(e);
    }
    if (!S_ISDIR(st.st_mode)) {
        free(root);
        return FILECAT_ERR_INVALID_ARG;
    }

    filecat_watcher_t *w = (filecat_watcher_t *)calloc(1, sizeof(*w));
    if (!w) { free(root); return FILECAT_ERR_NO_MEMORY; }

    w->root      = root;
    w->root_len  = strlen(root);
    w->recursive = recursive ? 1 : 0;
    atomic_init(&w->refcount,  1);
    atomic_init(&w->closing,   0);
    atomic_init(&w->destroyed, 0);

    if (pthread_mutex_init(&w->mu, NULL) != 0) {
        free(w->root); free(w);
        return FILECAT_ERR_SYSTEM;
    }
    if (pthread_cond_init(&w->cv, NULL) != 0) {
        pthread_mutex_destroy(&w->mu);
        free(w->root); free(w);
        return FILECAT_ERR_SYSTEM;
    }

    /* CFArray of one CFString — the path to watch. */
    CFStringRef cf_root = CFStringCreateWithCString(NULL, w->root,
                                                    kCFStringEncodingUTF8);
    if (!cf_root) {
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w->root); free(w);
        return FILECAT_ERR_NO_MEMORY;
    }
    CFArrayRef paths_array = CFArrayCreate(NULL, (const void **)&cf_root, 1,
                                           &kCFTypeArrayCallBacks);
    CFRelease(cf_root);
    if (!paths_array) {
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w->root); free(w);
        return FILECAT_ERR_NO_MEMORY;
    }

    /* NoDefer:        deliver the first batch with no latency window.
     * FileEvents:     per-file granularity so we can map flags to
     *                 CREATED / REMOVED / MODIFIED / RENAMED (10.7+).
     * UseCFTypes:     callback's eventPaths is a CFArrayRef (not char**).
     *                 Required by UseExtendedData — Apple's docs say
     *                 "implies" in places and "must be paired with" in
     *                 others; on the CI image FSEventStreamCreate
     *                 returns NULL without it, so we set both explicitly.
     * UseExtendedData: callback receives CFArray<CFDictionary> with the
     *                 path AND the inode per event (10.13+) — the inode
     *                 becomes event.event_correlation_id. */
    FSEventStreamContext ctx = {0, w, NULL, NULL, NULL};
    FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagNoDefer
                                   | kFSEventStreamCreateFlagFileEvents
                                   | kFSEventStreamCreateFlagUseCFTypes
                                   | kFSEventStreamCreateFlagUseExtendedData;
    w->stream = FSEventStreamCreate(NULL, fsevents_cb, &ctx, paths_array,
                                    kFSEventStreamEventIdSinceNow,
                                    0.0, flags);
    CFRelease(paths_array);
    if (!w->stream) {
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w->root); free(w);
        return FILECAT_ERR_SYSTEM;
    }

    w->queue = dispatch_queue_create("com.filecat.fsevents",
                                     DISPATCH_QUEUE_SERIAL);
    if (!w->queue) {
        FSEventStreamRelease(w->stream);
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w->root); free(w);
        return FILECAT_ERR_NO_MEMORY;
    }

    FSEventStreamSetDispatchQueue(w->stream, w->queue);
    if (!FSEventStreamStart(w->stream)) {
        FSEventStreamInvalidate(w->stream);
        FSEventStreamRelease(w->stream);
        dispatch_release(w->queue);
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w->root); free(w);
        return FILECAT_ERR_SYSTEM;
    }

    *out = w;
    return FILECAT_OK;
}

filecat_status_t filecat_next_event(filecat_watcher_t *w, filecat_event_t *out)
{
    if (!w || !out) return FILECAT_ERR_INVALID_ARG;
    watcher_retain(w);

    pthread_mutex_lock(&w->mu);
    for (;;) {
        if (atomic_load_explicit(&w->closing, memory_order_acquire)) {
            pthread_mutex_unlock(&w->mu);
            watcher_release(w);
            return FILECAT_ERR_CLOSED;
        }
        if (w->overflow_pending) {
            w->overflow_pending = 0;
            pthread_mutex_unlock(&w->mu);
            watcher_release(w);
            return FILECAT_ERR_OVERFLOW;
        }
        if (w->head) break;
        pthread_cond_wait(&w->cv, &w->mu);
    }

    struct fc_node *n = w->head;
    w->head = n->next;
    if (w->head == NULL) w->tail = NULL;
    pthread_mutex_unlock(&w->mu);

    /* Materialize into the watcher-owned scratch buffer. utf8_path is
     * touched only by the single consumer per the API contract, so we
     * don't need the mutex here. */
    size_t need = strlen(n->rel_path) + 1;
    if (need > w->utf8_capacity) {
        char *p = (char *)realloc(w->utf8_path, need);
        if (!p) {
            free(n->rel_path);
            free(n);
            watcher_release(w);
            return FILECAT_ERR_NO_MEMORY;
        }
        w->utf8_path     = p;
        w->utf8_capacity = need;
    }
    memcpy(w->utf8_path, n->rel_path, need);

    out->type                 = n->type;
    out->path                 = w->utf8_path;
    out->event_correlation_id = n->inode;

    free(n->rel_path);
    free(n);

    watcher_release(w);
    return FILECAT_OK;
}

void filecat_close(filecat_watcher_t *w)
{
    if (!w) return;
    watcher_retain(w);
    watcher_close_internal(w);
    watcher_release(w);
}

void filecat_destroy(filecat_watcher_t *w)
{
    if (!w) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&w->destroyed, &expected, 1)) return;
    watcher_close_internal(w);
    watcher_release(w);   /* drop owner ref */
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
