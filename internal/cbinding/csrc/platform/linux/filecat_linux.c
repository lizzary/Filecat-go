filecat_mac.c#define _GNU_SOURCE 1

#include "filecat/filecat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

/* 64 KB: mirrors the Windows backend's FILECAT_BUFFER_SIZE. Max single
 * inotify_event size is sizeof(struct inotify_event) + NAME_MAX + 1, so
 * this fits many records per read. */
#define FILECAT_BUFFER_SIZE (64u * 1024u)

#define FILECAT_WATCH_MASK                                                  \
    ( IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB                         \
    | IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF | IN_DELETE_SELF           \
    | IN_EXCL_UNLINK | IN_ONLYDIR )

/* Open-addressed (wd -> relative path) map. wd values:
 *   -1 = empty slot, -2 = tombstone, >=0 = live entry */
struct fc_wd_slot {
    int   wd;
    char *path;
};

struct filecat_watcher {
    int    inotify_fd;
    int    cancel_fd;             /* eventfd; signal -> wake the consumer */
    int    recursive;
    char  *root;                  /* canonical absolute path (no trailing /) */
    size_t root_len;

    struct fc_wd_slot *wds;
    size_t             wds_cap;   /* power of two */
    size_t             wds_valid;
    size_t             wds_used;  /* valid + tombstones */

    char  *buffer;                /* inotify read buffer (FILECAT_BUFFER_SIZE) */
    size_t buffer_len;
    size_t buffer_offset;

    char  *utf8_path;             /* scratch returned via event.path */
    size_t utf8_capacity;

    /* Lifecycle: mirror the Windows backend.
     *   refcount = 1 owner + 1 per in-flight call; last release frees.
     *   closing  = set-once latch flipped by close/destroy; gates the
     *              cancel-eventfd write so it runs exactly once.
     *   destroyed = set-once latch flipped by destroy; gates the owner
     *               ref drop so destroy is safe to race with itself. */
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
        case EBADF:
            return FILECAT_ERR_CLOSED;
        case EINVAL:
            return FILECAT_ERR_INVALID_ARG;
        default:
            return FILECAT_ERR_SYSTEM;
    }
}

/* ---- path helpers ------------------------------------------------------ */

static char *path_join(const char *parent, const char *name)
{
    size_t pl = parent ? strlen(parent) : 0;
    size_t nl = strlen(name);
    char *out;
    if (pl == 0) {
        out = (char *)malloc(nl + 1);
        if (!out) return NULL;
        memcpy(out, name, nl + 1);
    } else {
        out = (char *)malloc(pl + 1 + nl + 1);
        if (!out) return NULL;
        memcpy(out, parent, pl);
        out[pl] = '/';
        memcpy(out + pl + 1, name, nl + 1);
    }
    return out;
}

static char *build_abs_path(const filecat_watcher_t *w, const char *rel)
{
    size_t rl = rel ? strlen(rel) : 0;
    if (rl == 0) {
        char *out = (char *)malloc(w->root_len + 1);
        if (!out) return NULL;
        memcpy(out, w->root, w->root_len + 1);
        return out;
    }
    /* realpath() strips trailing slashes EXCEPT for the filesystem root "/".
     * Without this check we'd produce "//foo" when watching "/". */
    int needs_sep = !(w->root_len == 1 && w->root[0] == '/');
    size_t total = w->root_len + (size_t)needs_sep + rl + 1;
    char *out = (char *)malloc(total);
    if (!out) return NULL;
    memcpy(out, w->root, w->root_len);
    if (needs_sep) out[w->root_len] = '/';
    memcpy(out + w->root_len + needs_sep, rel, rl + 1);
    return out;
}

/* ---- wd map ------------------------------------------------------------ */

static size_t wd_slot_index(const struct fc_wd_slot *slots, size_t cap, int wd)
{
    size_t mask = cap - 1;
    size_t i = ((size_t)(unsigned)wd) & mask;
    size_t tomb = (size_t)-1;
    for (;;) {
        const struct fc_wd_slot *s = &slots[i];
        if (s->wd == wd) return i;
        if (s->wd == -1) return (tomb != (size_t)-1) ? tomb : i;
        if (s->wd == -2 && tomb == (size_t)-1) tomb = i;
        i = (i + 1) & mask;
    }
}

static int wd_map_init(filecat_watcher_t *w)
{
    size_t cap = 16;
    w->wds = (struct fc_wd_slot *)malloc(sizeof(*w->wds) * cap);
    if (!w->wds) return -1;
    for (size_t i = 0; i < cap; i++) {
        w->wds[i].wd = -1;
        w->wds[i].path = NULL;
    }
    w->wds_cap = cap;
    w->wds_valid = 0;
    w->wds_used = 0;
    return 0;
}

static int wd_map_grow(filecat_watcher_t *w)
{
    size_t new_cap = w->wds_cap * 2;
    struct fc_wd_slot *ns = (struct fc_wd_slot *)malloc(sizeof(*ns) * new_cap);
    if (!ns) return -1;
    for (size_t i = 0; i < new_cap; i++) {
        ns[i].wd = -1;
        ns[i].path = NULL;
    }
    for (size_t i = 0; i < w->wds_cap; i++) {
        if (w->wds[i].wd < 0) continue;
        size_t idx = wd_slot_index(ns, new_cap, w->wds[i].wd);
        ns[idx] = w->wds[i];
    }
    free(w->wds);
    w->wds = ns;
    w->wds_cap = new_cap;
    w->wds_used = w->wds_valid;   /* tombstones dropped on rehash */
    return 0;
}

static int wd_map_put(filecat_watcher_t *w, int wd, char *path)
{
    if ((w->wds_used + 1) * 4 > w->wds_cap * 3) {
        if (wd_map_grow(w) != 0) return -1;
    }
    size_t i = wd_slot_index(w->wds, w->wds_cap, wd);
    if (w->wds[i].wd == wd) {
        free(w->wds[i].path);
        w->wds[i].path = path;
        return 0;
    }
    int was_tomb = (w->wds[i].wd == -2);
    w->wds[i].wd = wd;
    w->wds[i].path = path;
    w->wds_valid++;
    if (!was_tomb) w->wds_used++;
    return 0;
}

static const char *wd_map_get(const filecat_watcher_t *w, int wd)
{
    if (wd < 0) return NULL;
    size_t i = wd_slot_index(w->wds, w->wds_cap, wd);
    return (w->wds[i].wd == wd) ? w->wds[i].path : NULL;
}

static void wd_map_remove(filecat_watcher_t *w, int wd)
{
    if (wd < 0) return;
    size_t i = wd_slot_index(w->wds, w->wds_cap, wd);
    if (w->wds[i].wd != wd) return;
    free(w->wds[i].path);
    w->wds[i].path = NULL;
    w->wds[i].wd = -2;   /* tombstone */
    w->wds_valid--;
}

static void wd_map_free(filecat_watcher_t *w)
{
    if (!w->wds) return;
    for (size_t i = 0; i < w->wds_cap; i++) free(w->wds[i].path);
    free(w->wds);
    w->wds = NULL;
}

/* ---- watch creation + recursive walk ----------------------------------- */

static int add_one_watch(filecat_watcher_t *w, const char *rel)
{
    char *abs = build_abs_path(w, rel);
    if (!abs) { errno = ENOMEM; return -1; }
    int wd = inotify_add_watch(w->inotify_fd, abs, FILECAT_WATCH_MASK);
    free(abs);
    if (wd < 0) return -1;

    char *copy = strdup(rel ? rel : "");
    if (!copy) {
        inotify_rm_watch(w->inotify_fd, wd);
        errno = ENOMEM;
        return -1;
    }
    if (wd_map_put(w, wd, copy) != 0) {
        inotify_rm_watch(w->inotify_fd, wd);
        free(copy);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static void descend_and_watch(filecat_watcher_t *w, const char *rel);

/* Add a watch for `rel`, then (if recursive) descend. Best effort: failures
 * on descendants are silently dropped — partial trees beat aborting the
 * watcher because /proc/sys/fs/inotify/max_user_watches was hit somewhere
 * deep in the tree. */
static void watch_subtree(filecat_watcher_t *w, const char *rel)
{
    if (add_one_watch(w, rel) != 0) return;
    if (w->recursive) descend_and_watch(w, rel);
}

static void descend_and_watch(filecat_watcher_t *w, const char *rel)
{
    char *abs = build_abs_path(w, rel);
    if (!abs) return;
    DIR *d = opendir(abs);
    free(abs);
    if (!d) return;

    for (struct dirent *de; (de = readdir(d)) != NULL; ) {
        const char *n = de->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        /* DT_LNK and other non-dir types: don't descend (avoid symlink loops). */
        if (de->d_type != DT_DIR && de->d_type != DT_UNKNOWN) continue;

        char *crel = path_join(rel, n);
        if (!crel) continue;

        /* Some filesystems (XFS, older ones over NFS) return DT_UNKNOWN.
         * Fall back to lstat in that case; reuse crel for the inotify call. */
        if (de->d_type == DT_UNKNOWN) {
            char *full = build_abs_path(w, crel);
            int is_dir = 0;
            if (full) {
                struct stat st;
                if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = 1;
                free(full);
            }
            if (!is_dir) { free(crel); continue; }
        }
        watch_subtree(w, crel);
        free(crel);
    }
    closedir(d);
}

/* ---- refcount + close latches ------------------------------------------
 *
 * Lifetime model (mirror of Windows backend):
 *   - filecat_open initializes refcount=1 (the owner ref).
 *   - filecat_next_event / filecat_close retain at entry, release at exit.
 *   - filecat_destroy CAS-sets the `destroyed` latch (so only one call
 *     drops the owner ref) and then releases.
 *   - The last release does the real free.
 *
 * `closing` is a set-once latch separate from `destroyed`: filecat_close
 * flips `closing` (without touching `destroyed`); filecat_destroy flips
 * both. The eventfd write (which unblocks the consumer's poll) is gated by
 * `closing` so it runs exactly once.
 */

static void watcher_free(filecat_watcher_t *w)
{
    if (w->inotify_fd >= 0) close(w->inotify_fd);
    if (w->cancel_fd  >= 0) close(w->cancel_fd);
    wd_map_free(w);
    free(w->buffer);
    free(w->utf8_path);
    free(w->root);
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

static void watcher_close_internal(filecat_watcher_t *w)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&w->closing, &expected, 1)) return;
    /* Wake any consumer parked in poll(). The fd may already be drained or
     * full; either way the consumer wakes. */
    uint64_t one = 1;
    ssize_t r;
    do { r = write(w->cancel_fd, &one, sizeof(one)); }
    while (r < 0 && errno == EINTR);
    (void)r;
}

/* ---- I/O helpers ------------------------------------------------------- */

/* Write `parent + "/" + name` (or just `name` if parent is empty) directly
 * into the watcher-owned scratch buffer, growing it as needed. Avoids the
 * per-event malloc/free that an intermediate path_join would cost on the
 * hot path of filecat_next_event. */
static filecat_status_t store_event_path_joined(filecat_watcher_t *w,
                                                const char *parent,
                                                const char *name)
{
    size_t pl = parent ? strlen(parent) : 0;
    size_t nl = strlen(name);
    size_t need = (pl == 0) ? nl + 1 : pl + 1 + nl + 1;

    if (need > w->utf8_capacity) {
        char *p = (char *)realloc(w->utf8_path, need);
        if (!p) return FILECAT_ERR_NO_MEMORY;
        w->utf8_path = p;
        w->utf8_capacity = need;
    }
    if (pl == 0) {
        memcpy(w->utf8_path, name, nl + 1);
    } else {
        memcpy(w->utf8_path, parent, pl);
        w->utf8_path[pl] = '/';
        memcpy(w->utf8_path + pl + 1, name, nl + 1);
    }
    return FILECAT_OK;
}

static filecat_event_type_t map_inotify_mask(uint32_t mask)
{
    /* Order matters: a single event may set IN_MOVED_FROM | IN_ISDIR
     * together with IN_ATTRIB; the move flag wins. */
    if (mask & IN_MOVED_FROM) return FILECAT_EVENT_RENAMED_OLD;
    if (mask & IN_MOVED_TO)   return FILECAT_EVENT_RENAMED_NEW;
    if (mask & IN_CREATE)     return FILECAT_EVENT_CREATED;
    if (mask & IN_DELETE)     return FILECAT_EVENT_REMOVED;
    if (mask & (IN_MODIFY | IN_ATTRIB)) return FILECAT_EVENT_MODIFIED;
    return FILECAT_EVENT_MODIFIED;
}

/* Block on poll(inotify_fd, cancel_fd) and refill the read buffer.
 * Returns FILECAT_OK and sets buffer_len on success. */
static filecat_status_t fill_buffer(filecat_watcher_t *w)
{
    for (;;) {
        if (atomic_load_explicit(&w->closing, memory_order_acquire) != 0)
            return FILECAT_ERR_CLOSED;

        struct pollfd fds[2];
        fds[0].fd = w->inotify_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = w->cancel_fd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        if (fds[1].revents & POLLIN) return FILECAT_ERR_CLOSED;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            return FILECAT_ERR_CLOSED;
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(w->inotify_fd, w->buffer, FILECAT_BUFFER_SIZE);
            if (n < 0) {
                if (errno == EINTR) continue;
                return map_errno(errno);
            }
            if (n == 0) continue;   /* shouldn't happen for inotify */
            w->buffer_len = (size_t)n;
            w->buffer_offset = 0;
            return FILECAT_OK;
        }
        /* poll returned for some other reason; loop. */
    }
}

/* ---- public API -------------------------------------------------------- */

filecat_status_t filecat_open(const char *path, int recursive, filecat_watcher_t **out)
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
    if (!w) {
        free(root);
        return FILECAT_ERR_NO_MEMORY;
    }
    w->inotify_fd = -1;
    w->cancel_fd  = -1;
    w->root       = root;
    w->root_len   = strlen(root);
    w->recursive  = recursive ? 1 : 0;
    atomic_init(&w->refcount,  1);
    atomic_init(&w->closing,   0);
    atomic_init(&w->destroyed, 0);

    w->inotify_fd = inotify_init1(IN_CLOEXEC);
    if (w->inotify_fd < 0) {
        filecat_status_t s = map_errno(errno);
        watcher_free(w);
        return s;
    }
    w->cancel_fd = eventfd(0, EFD_CLOEXEC);
    if (w->cancel_fd < 0) {
        filecat_status_t s = map_errno(errno);
        watcher_free(w);
        return s;
    }
    w->buffer = (char *)malloc(FILECAT_BUFFER_SIZE);
    if (!w->buffer) {
        watcher_free(w);
        return FILECAT_ERR_NO_MEMORY;
    }
    if (wd_map_init(w) != 0) {
        watcher_free(w);
        return FILECAT_ERR_NO_MEMORY;
    }

    /* Root watch is mandatory; descendants are best-effort. */
    if (add_one_watch(w, "") != 0) {
        filecat_status_t s = map_errno(errno);
        watcher_free(w);
        return s;
    }
    if (w->recursive) descend_and_watch(w, "");

    *out = w;
    return FILECAT_OK;
}

filecat_status_t filecat_next_event(filecat_watcher_t *w, filecat_event_t *out)
{
    if (!w || !out) return FILECAT_ERR_INVALID_ARG;
    watcher_retain(w);

    filecat_status_t status = FILECAT_OK;

    for (;;) {
        if (atomic_load_explicit(&w->closing, memory_order_acquire) != 0) {
            status = FILECAT_ERR_CLOSED;
            goto out_release;
        }
        if (w->buffer_offset >= w->buffer_len) {
            status = fill_buffer(w);
            if (status != FILECAT_OK) goto out_release;
        }

        const struct inotify_event *ev =
            (const struct inotify_event *)(w->buffer + w->buffer_offset);
        w->buffer_offset += sizeof(struct inotify_event) + ev->len;

        if (ev->mask & IN_Q_OVERFLOW) {
            status = FILECAT_ERR_OVERFLOW;
            goto out_release;
        }
        if (ev->mask & IN_IGNORED) {
            wd_map_remove(w, ev->wd);
            continue;
        }
        /* len == 0 covers SELF events (IN_DELETE_SELF / IN_MOVE_SELF /
         * IN_ATTRIB on the watched dir itself) — they carry no name. We
         * skip them: the parent's IN_DELETE / IN_MOVED_FROM already covers
         * the change when there is a parent watch, and for the root this
         * mirrors Windows ReadDirectoryChangesW, which never reports
         * changes to the watched dir itself. */
        if (ev->len == 0) continue;

        const char *parent_rel = wd_map_get(w, ev->wd);
        if (!parent_rel) continue;   /* unknown wd (just rm_watch'd) */

        /* Materialize the relative path into the watcher-owned scratch
         * buffer ONCE and reuse it below. utf8_path stays stable across
         * watch_subtree / wd-map iteration (neither touches it). */
        filecat_status_t s = store_event_path_joined(w, parent_rel, ev->name);
        if (s != FILECAT_OK) { status = s; goto out_release; }
        const char *rel = w->utf8_path;

        if (w->recursive && (ev->mask & IN_ISDIR)) {
            if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
                /* New subdirectory: install watches on it (and its
                 * descendants, if it was pre-populated). add_one_watch
                 * strdup's `rel`, so the scratch buffer can keep aliasing
                 * it for the event we're about to return. */
                watch_subtree(w, rel);
            }
            if (ev->mask & IN_MOVED_FROM) {
                /* The subtree moved (within the tree or out of it). The
                 * inode-keyed watches stay valid but their paths in our
                 * map are stale, so drop them. IN_IGNORED arrives later
                 * to clear the map entries; if the dir was moved back in
                 * via IN_MOVED_TO we'll re-add fresh watches above.
                 * Known limitation: a small window of events with stale
                 * paths may slip out between the actual rename and our
                 * processing of IN_MOVED_FROM. */
                size_t plen = strlen(rel);
                for (size_t i = 0; i < w->wds_cap; i++) {
                    if (w->wds[i].wd < 0) continue;
                    const char *p = w->wds[i].path;
                    if (strcmp(p, rel) == 0 ||
                        (strncmp(p, rel, plen) == 0 && p[plen] == '/')) {
                        inotify_rm_watch(w->inotify_fd, w->wds[i].wd);
                    }
                }
            }
        }

        out->type = map_inotify_mask(ev->mask);
        out->path = w->utf8_path;
        /* Non-zero only on IN_MOVED_FROM / IN_MOVED_TO; the two halves of a
         * single rename(2) share the same cookie. Downstream keys its
         * rename-pairing hashmap on this. */
        out->event_correlation_id = (uint64_t)ev->cookie;
        status = FILECAT_OK;
        goto out_release;
    }

out_release:
    watcher_release(w);
    return status;
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
