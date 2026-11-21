/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SKSE_SAVE_DIRECTORY_LIMIT = 100000 };

typedef struct {
    char base[NAME_MAX + 1], side[NAME_MAX + 1];
} SkseSaveNames;

static inline unsigned skse_save_ascii_fold(unsigned c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static inline int skse_save_ascii_equal(const char *a, const char *b, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (skse_save_ascii_fold((unsigned char)a[i]) != skse_save_ascii_fold((unsigned char)b[i]))
            return 0;
    return 1;
}

static inline size_t skse_save_stem(const char *name) {
    if (!name || name[0] == '.') return 0;
    size_t n = strnlen(name, NAME_MAX + 1);
    if (n <= 4 || n > NAME_MAX || !skse_save_ascii_equal(name + n - 4, ".ess", 4)) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < ' ' || c == 127 || c == '/' || c == '\\') return 0;
    }
    return n - 4;
}

/* Use a fresh file description so scans never consume a caller's directory position. */
static inline int skse_save_find_names(int dir, const char *base, size_t stem, SkseSaveNames *names) {
    *names = (SkseSaveNames){0};
    int scan = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (scan < 0) return errno;
    DIR *list = fdopendir(scan);
    if (!list) {
        int error = errno;
        close(scan);
        return error;
    }
    int error = 0;
    unsigned seen = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(list);
        if (!entry) { error = errno; break; }
        if (++seen > SKSE_SAVE_DIRECTORY_LIMIT) { error = E2BIG; break; }
        const char *name = entry->d_name;
        size_t n = strlen(name);
        if ((n != stem + 4 && n != stem + 5) || n > NAME_MAX || !skse_save_ascii_equal(name, base, stem))
            continue;
        char *target = NULL;
        if (n == stem + 4 && skse_save_ascii_equal(name + stem, ".ess", 4)) target = names->base;
        if (n == stem + 5 && skse_save_ascii_equal(name + stem, ".skse", 5)) target = names->side;
        if (!target) continue;
        if (*target) { error = EEXIST; break; }
        memcpy(target, name, n + 1);
    }
    if (closedir(list) && !error) error = errno;
    return error;
}

static inline int skse_save_same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static inline int skse_save_same_version(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
    const struct timespec am = a->st_mtimespec, bm = b->st_mtimespec, ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
    const struct timespec am = a->st_mtim, bm = b->st_mtim, ac = a->st_ctim, bc = b->st_ctim;
#endif
    return skse_save_same_file(a, b) && a->st_mode == b->st_mode && a->st_size == b->st_size &&
           a->st_nlink == b->st_nlink && am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec &&
           ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}
