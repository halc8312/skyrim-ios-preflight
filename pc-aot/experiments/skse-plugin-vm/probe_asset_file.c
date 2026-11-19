/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "probe_asset_file.h"
#include "x86_winfile.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { FILE_MISSING = 2, PATH_MISSING = 3, DENIED = 5, NO_MEMORY = 8, INCOMPLETE = 13, TOO_LARGE = 223 };
enum { MAX_CASE_SCAN_ENTRIES = 4096 };

/* Returns the width of a Unicode scalar encoded in UTF-8. */
static unsigned utf8_width(const unsigned char *s, size_t left) {
    if (s[0] < 0x80) return 1;
    unsigned width = s[0] <= 0xdf ? 2 : s[0] <= 0xef ? 3 : 4;
    if (s[0] < 0xc2 || s[0] > 0xf4 || width > left) return 0;
    for (unsigned i = 1; i < width; ++i)
        if ((s[i] & 0xc0) != 0x80) return 0;
    if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0) ||
        (s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] >= 0x90)) return 0;
    return width;
}

static bool normalized_path(const char *path) {
    if (!path) return false;
    const size_t length = strnlen(path, SKSE_WIN_PATH_BYTES);
    if (!length || length == SKSE_WIN_PATH_BYTES) return false;
    size_t units = 0, start = 0;
    for (size_t i = 0; i < length;) {
        unsigned char c = (unsigned char)path[i];
        if (c == '/') {
            if (i == start || i - start > NAME_MAX || path[i-1] == ' ' || path[i-1] == '.') return false;
            start = ++i;
            ++units;
            continue;
        }
        if (c < 0x20 || c == 0x7f || strchr("\\:\"<>|?*", c)) return false;
        unsigned width = utf8_width((const unsigned char *)path + i, length - i);
        if (!width) return false;
        units += width == 4 ? 2 : 1;
        i += width;
    }
    return units < SKSE_WIN_PATH_UNITS && length > start && length - start <= NAME_MAX &&
           path[length-1] != ' ' && path[length-1] != '.';
}

static uint32_t lookup_error(int error, uint32_t missing) {
    switch (error) {
        case ENOENT: return missing;
        case ENOTDIR: return PATH_MISSING;
        case ELOOP: case EACCES: case EPERM: case EBADF: case ENAMETOOLONG: return DENIED;
        case ENOMEM: case EMFILE: case ENFILE: return NO_MEMORY;
        default: return INCOMPLETE;
    }
}

/* Keep the exact-name path to one stat. A fallback has an independent
 * directory offset and may only choose an unambiguous ASCII case match. */
static uint32_t lookup_component(int parent, char name[NAME_MAX + 1], struct stat *info,
                                 uint32_t missing, unsigned *remaining) {
    if (fstatat(parent, name, info, AT_SYMLINK_NOFOLLOW) == 0) return 0;
    if (errno != ENOENT) return lookup_error(errno, missing);
    int scan = openat(parent, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
    if (scan < 0) return lookup_error(errno, missing);
    DIR *entries = fdopendir(scan);
    if (!entries) {
        uint32_t result = lookup_error(errno, INCOMPLETE);
        close(scan);
        return result;
    }
    char match[NAME_MAX + 1] = {0};
    const size_t name_bytes = strlen(name) + 1;
    uint32_t result = missing;
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(entries);
        if (!entry) {
            if (errno) result = lookup_error(errno, INCOMPLETE);
            break;
        }
        if (!*remaining) { result = INCOMPLETE; break; }
        --*remaining;
        if (!skse_win_asset_name_equal(name, entry->d_name)) continue;
        if (*match) { result = DENIED; break; }
        // A match has the same byte length as the verified component.
        memcpy(match, entry->d_name, name_bytes);
        result = 0;
    }
    closedir(entries);
    if (result) return result;
    if (fstatat(parent, match, info, AT_SYMLINK_NOFOLLOW) < 0) return lookup_error(errno, missing);
    memcpy(name, match, name_bytes);
    return 0;
}

static bool same_object(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool regular_asset(const struct stat *s, dev_t device) {
    return S_ISREG(s->st_mode) && s->st_dev == device && s->st_nlink == 1;
}

static bool unchanged(const struct stat *a, const struct stat *b) {
    if (!same_object(a, b) || !regular_asset(b, a->st_dev) || a->st_mode != b->st_mode ||
        a->st_size != b->st_size) return false;
#if defined(__APPLE__)
    const struct timespec am = a->st_mtimespec, bm = b->st_mtimespec;
    const struct timespec ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
    const struct timespec am = a->st_mtim, bm = b->st_mtim;
    const struct timespec ac = a->st_ctim, bc = b->st_ctim;
#endif
    return am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec &&
           ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

/* Pin each directory while opening its next component. Return an owned parent
 * descriptor even if the final file is directly inside the selected root. */
static uint32_t open_parent(int root, dev_t device, const char *path, int *parent,
                            char leaf[NAME_MAX + 1], unsigned *remaining) {
    int fd = fcntl(root, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return lookup_error(errno, PATH_MISSING);
    uint32_t result = 0;
    for (const char *slash; (slash = strchr(path, '/')); path = slash + 1) {
        char part[NAME_MAX + 1];
        size_t n = (size_t)(slash - path);
        memcpy(part, path, n);
        part[n] = 0;
        struct stat listed, opened;
        result = lookup_component(fd, part, &listed, PATH_MISSING, remaining);
        if (result) break;
        if (!S_ISDIR(listed.st_mode) || listed.st_dev != device) {
            result = S_ISREG(listed.st_mode) ? PATH_MISSING : DENIED;
            break;
        }
        int next = openat(fd, part, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
        if (next < 0) {
            result = lookup_error(errno, INCOMPLETE);
            break;
        }
        close(fd);
        fd = next;
        if (fstat(fd, &opened) < 0 || !same_object(&listed, &opened) || !S_ISDIR(opened.st_mode)) {
            result = INCOMPLETE;
            break;
        }
    }
    if (result) close(fd);
    else {
        *parent = fd;
        memcpy(leaf, path, strlen(path) + 1);
    }
    return result;
}

static ssize_t read_retry(int fd, uint8_t *out, size_t size) {
    ssize_t n;
    do { n = read(fd, out, size); } while (n < 0 && errno == EINTR);
    return n;
}

static uint32_t snapshot_file(int fd, const struct stat *before, size_t max_size,
                              uint8_t **candidate, size_t *size) {
    const uintmax_t n = (uintmax_t)before->st_size;
    if (before->st_size < 0) return INCOMPLETE;
    if (n > max_size) return TOO_LARGE;
    *size = (size_t)n;
    *candidate = malloc(*size ? *size : 1);
    if (!*candidate) return NO_MEMORY;
    size_t at = 0;
    while (at < *size) {
        size_t remaining = *size - at;
        ssize_t got = read_retry(fd, *candidate + at, remaining < (size_t)SSIZE_MAX ? remaining : (size_t)SSIZE_MAX);
        if (got <= 0) return INCOMPLETE;
        at += (size_t)got;
    }
    uint8_t extra;
    struct stat after;
    if (read_retry(fd, &extra, 1) != 0 || fstat(fd, &after) < 0 || !unchanged(before, &after)) return INCOMPLETE;
    return 0;
}

static int authorized_directory(void *context, struct stat *directory) {
    if (!context) return -1;
    const int root = *(const int *)context;
    const int flags = fcntl(root, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(root, directory) < 0 ||
        !S_ISDIR(directory->st_mode)) return -1;
    return root;
}

uint32_t skse_probe_asset_read(void *context, const char *path, size_t max_size,
                               uint8_t **bytes, size_t *length) {
    if (bytes) *bytes = NULL;
    if (length) *length = 0;
    if (!bytes || !length || !normalized_path(path)) return DENIED;
    struct stat directory;
    int root = authorized_directory(context, &directory);
    if (root < 0) return DENIED;

    int parent = -1, file = -1;
    char leaf[NAME_MAX + 1];
    unsigned remaining = MAX_CASE_SCAN_ENTRIES;
    uint8_t *candidate = NULL;
    size_t size = 0;
    struct stat listed, opened, named;
    uint32_t result = open_parent(root, directory.st_dev, path, &parent, leaf, &remaining);
    if (result) return result;
    result = lookup_component(parent, leaf, &listed, FILE_MISSING, &remaining);
    if (result) {
        close(parent);
        return result;
    }
    if (!regular_asset(&listed, directory.st_dev)) {
        result = DENIED;
    } else if ((file = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)) < 0) {
        result = lookup_error(errno, INCOMPLETE);
    } else if (fstat(file, &opened) < 0 || !unchanged(&listed, &opened)) {
        result = INCOMPLETE;
    } else {
        result = snapshot_file(file, &opened, max_size, &candidate, &size);
    }
    close(parent);
    if (file >= 0) {
        /* Verify the name again starting at the selected root. A renamed
         * ancestor must not allow publishing a file from a detached folder. */
        if (!result) {
            result = INCOMPLETE;
            if (!open_parent(root, directory.st_dev, path, &parent, leaf, &remaining)) {
                if (!lookup_component(parent, leaf, &named, FILE_MISSING, &remaining) && unchanged(&opened, &named))
                    result = 0;
                close(parent);
            }
        }
        if (close(file) < 0 && !result) result = INCOMPLETE;
    }
    if (result) free(candidate);
    else {
        *bytes = candidate;
        *length = size;
    }
    return result;
}

uint32_t skse_probe_asset_attributes(void *context, const char *path, uint32_t *attributes) {
    if (!attributes) return DENIED;
    *attributes = 0;
    if (!normalized_path(path)) return DENIED;
    struct stat directory;
    const int root = authorized_directory(context, &directory);
    if (root < 0) return DENIED;
    int parent;
    char leaf[NAME_MAX + 1];
    unsigned remaining = MAX_CASE_SCAN_ENTRIES;
    uint32_t result = open_parent(root, directory.st_dev, path, &parent, leaf, &remaining);
    if (result) return result;
    struct stat listed;
    result = lookup_component(parent, leaf, &listed, FILE_MISSING, &remaining);
    close(parent);
    if (result) return result;
    if (S_ISDIR(listed.st_mode) && listed.st_dev == directory.st_dev) {
        *attributes = SKSE_WIN_ATTRIBUTE_DIRECTORY;
    } else if (regular_asset(&listed, directory.st_dev)) {
        *attributes = SKSE_WIN_ATTRIBUTE_NORMAL;
    } else {
        result = DENIED;
    }
    return result;
}
