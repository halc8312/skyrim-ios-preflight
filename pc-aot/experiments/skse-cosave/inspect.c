/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "inspect.h"
#include "records.h"
#include "save_paths.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { READ_AHEAD = 4096 };
typedef struct {
    int fd;
    size_t size, offset, filled;
    uint8_t buffer[READ_AHEAD];
} Reader;

static int valid_directory(const char *path) {
    if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) >= PATH_MAX) return 0;
    for (const char *p = path + 1; *p;) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (!n || n > NAME_MAX || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        if (!slash) break;
        p = slash + 1;
    }
    return 1;
}

static int open_directory(const char *path) {
    if (!valid_directory(path)) {
        errno = EINVAL;
        return -1;
    }
#if defined(__APPLE__) && defined(O_NOFOLLOW_ANY)
    /* Resolve the authorized directory in one syscall. iOS grants access to
     * the container, but not necessarily permission to open its ancestors. */
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW_ANY);
#else
    /* O_NOFOLLOW by itself only protects the final component on POSIX. */
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        char name[NAME_MAX + 1];
        memcpy(name, p, n);
        name[n] = 0;
        int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = errno;
        close(fd);
        if (child < 0) {
            errno = error;
            return -1;
        }
        fd = child;
        if (!slash) break;
        p = slash + 1;
    }
    return fd;
#endif
}

static SkseInspectStatus find_names(int dir, const char *base, size_t stem, SkseSaveNames *names) {
    int error = skse_save_find_names(dir, base, stem, names);
    if (error == EEXIST) return SKSE_INSPECT_AMBIGUOUS;
    if (error == E2BIG) return SKSE_INSPECT_LIMIT;
    if (error) return SKSE_INSPECT_IO;
    if (!names->base[0]) return SKSE_INSPECT_BASE_MISSING;
    return names->side[0] ? SKSE_INSPECT_OK : SKSE_INSPECT_MISSING;
}
static SkseInspectStatus open_named(int dir, const char *name, int *fd, struct stat *initial) {
    struct stat named;
    if (fstatat(dir, name, &named, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? SKSE_INSPECT_UNSTABLE : SKSE_INSPECT_IO;
    if (!S_ISREG(named.st_mode) || named.st_nlink != 1) return SKSE_INSPECT_INVALID;
    *fd = openat(dir, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (*fd < 0) return errno == ENOENT || errno == ELOOP ? SKSE_INSPECT_UNSTABLE : SKSE_INSPECT_IO;
    if (fstat(*fd, initial)) return SKSE_INSPECT_IO;
    return skse_save_same_version(&named, initial) ? SKSE_INSPECT_OK : SKSE_INSPECT_UNSTABLE;
}
static int stable_name(int dir, const char *name, int fd, const struct stat *initial) {
    struct stat named, opened;
    return !fstatat(dir, name, &named, AT_SYMLINK_NOFOLLOW) && !fstat(fd, &opened) &&
           skse_save_same_version(initial, &named) && skse_save_same_version(initial, &opened);
}
static int buffered_read(void *context, size_t offset, void *out, size_t length) {
    Reader *r = context;
    if (length > sizeof r->buffer || offset > r->size || length > r->size - offset) return 0;
    if (offset < r->offset || offset - r->offset > r->filled || length > r->filled - (offset - r->offset)) {
        r->offset = offset;
        r->filled = 0;
        size_t goal = r->size - offset;
        if (goal > sizeof r->buffer) goal = sizeof r->buffer;
        while (r->filled < goal) {
            ssize_t n = pread(r->fd, r->buffer + r->filled, goal - r->filled, (off_t)(offset + r->filled));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return 0;
            r->filled += (size_t)n;
        }
    }
    memcpy(out, r->buffer + (offset - r->offset), length);
    return 1;
}
static SkseInspectStatus inspect_fd(int fd, const struct stat *st, SkseInspectInfo *out) {
    if (st->st_size < 0) return SKSE_INSPECT_IO;
    if ((uint64_t)st->st_size > SKSE_PAIR_MAX_COMPANION) return SKSE_INSPECT_LIMIT;
    Reader r = {.fd = fd, .size = (size_t)st->st_size};
    SkseRecordsSummary summary = {0};
    SksePairResult result = skse_records_inspect(r.size, buffered_read, &r, &summary);
    if (result == SKSE_PAIR_LIMIT) return SKSE_INSPECT_LIMIT;
    if (result == SKSE_PAIR_STORAGE) return SKSE_INSPECT_IO;
    if (result != SKSE_PAIR_OK && result != SKSE_PAIR_UNSUPPORTED) return SKSE_INSPECT_INVALID;
    *out = (SkseInspectInfo){
        r.size,         summary.format_version, summary.skse_version, summary.runtime_version,
        summary.owners, summary.records};
    return result == SKSE_PAIR_OK ? SKSE_INSPECT_OK : SKSE_INSPECT_UNSUPPORTED;
}

SkseInspectStatus skse_inspect_companion(const char *directory, const char *base, SkseInspectInfo *out) {
    if (!out) return SKSE_INSPECT_ARGUMENT;
    *out = (SkseInspectInfo){0};
    size_t stem = skse_save_stem(base);
    if (!stem || !directory || !*directory) return SKSE_INSPECT_ARGUMENT;
    int dir = open_directory(directory);
    if (dir < 0) return errno == EINVAL ? SKSE_INSPECT_ARGUMENT : SKSE_INSPECT_IO;
    SkseInspectStatus status = SKSE_INSPECT_IO;
    struct stat dir_before, dir_after, base_before, side_before;
    SkseSaveNames names;
    int base_fd = -1, side_fd = -1;
    SkseInspectInfo info = {0};
    if (!fstat(dir, &dir_before)) {
        status = find_names(dir, base, stem, &names);
        if (status == SKSE_INSPECT_OK || status == SKSE_INSPECT_MISSING) {
            status = open_named(dir, names.base, &base_fd, &base_before);
            if (status == SKSE_INSPECT_OK) {
                status = names.side[0] ? open_named(dir, names.side, &side_fd, &side_before)
                                       : SKSE_INSPECT_MISSING;
                if (status == SKSE_INSPECT_OK) {
                    status = inspect_fd(side_fd, &side_before, &info);
                    if (!stable_name(dir, names.side, side_fd, &side_before))
                        status = SKSE_INSPECT_UNSTABLE;
                }
                if (!stable_name(dir, names.base, base_fd, &base_before)) status = SKSE_INSPECT_UNSTABLE;
            }
        }
        int path_fd = open_directory(directory);
        if (path_fd < 0 || fstat(path_fd, &dir_after) || !skse_save_same_version(&dir_before, &dir_after))
            status = SKSE_INSPECT_UNSTABLE;
        if (path_fd >= 0) close(path_fd);
    }
    if (side_fd >= 0) close(side_fd);
    if (base_fd >= 0) close(base_fd);
    close(dir);
    if (status == SKSE_INSPECT_OK || status == SKSE_INSPECT_UNSUPPORTED) *out = info;
    return status;
}
