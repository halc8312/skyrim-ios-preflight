/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include "immutable_ess.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>
#elif defined(__linux__) && !defined(SKSE_TEST_NAMED_CLONE)
#include <linux/fs.h>
#include <stdio.h>
#include <sys/ioctl.h>
#endif

struct SkseImmutableEss { int fd, hashed; SkseEssDigest digest; };

#if defined(__APPLE__) || defined(__linux__) || defined(SKSE_TEST_NAMED_CLONE)
static SksePairResult clone_error(int error) {
    if (error == ENOTSUP || error == EOPNOTSUPP || error == EXDEV ||
        error == ENOTTY || error == ENOSYS) return SKSE_PAIR_UNSUPPORTED;
    if (error == EEXIST) return SKSE_PAIR_CONFLICT;
    return SKSE_PAIR_STORAGE;
}
#endif

#if defined(__APPLE__) || defined(SKSE_TEST_NAMED_CLONE)
#if defined(SKSE_TEST_NAMED_CLONE)
extern int skse_test_fclonefileat(int source, int dir, const char *name, unsigned flags);
#define SKSE_FCLONEFILEAT skse_test_fclonefileat
#else
#define SKSE_FCLONEFILEAT fclonefileat
#endif
static SksePairResult capture_file(int source, int dir, int *out) {
    static const char name[] = "base.clone";
    /* fclonefileat fails if a name already exists. Never open/remove such a
     * name. Only this successful call may unlink its own scratch clone. */
    if (SKSE_FCLONEFILEAT(source, dir, name, 0)) return clone_error(errno);
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (unlinkat(dir, name, 0) || fd < 0) {
        if (fd >= 0) close(fd);
        return SKSE_PAIR_STORAGE;
    }
    struct stat from, copy;
    if (fstat(source, &from) || fstat(fd, &copy) ||
        (from.st_dev == copy.st_dev && from.st_ino == copy.st_ino)) {
        close(fd);
        return SKSE_PAIR_STORAGE;
    }
    *out = fd;
    return SKSE_PAIR_OK;
}
#elif defined(__linux__)
static SksePairResult capture_file(int source, int dir, int *out) {
    /* A Linux host can also supply an already immutable anonymous file. The
     * three seals cannot be removed, even by a second writable descriptor. */
    int seals = fcntl(source, F_GET_SEALS);
    const int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    if (seals >= 0 && (seals & required) == required) {
        *out = fcntl(source, F_DUPFD_CLOEXEC, 0);
        return *out < 0 ? SKSE_PAIR_STORAGE : SKSE_PAIR_OK;
    }
    /* O_TMPFILE|O_EXCL makes the private inode anonymous and un-linkable.
     * The sole writable descriptor is closed before the capture escapes. */
    int writer = openat(dir, ".", O_TMPFILE | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (writer < 0) return clone_error(errno);
    int cloned = ioctl(writer, FICLONE, source);
    int error = errno;
    if (cloned == 0) {
        char path[64];
        int length = snprintf(path, sizeof path, "/proc/self/fd/%d", writer);
        if (length > 0 && (size_t)length < sizeof path) *out = open(path, O_RDONLY | O_CLOEXEC);
    }
    close(writer);
    if (cloned != 0) return clone_error(error);
    return *out < 0 ? SKSE_PAIR_STORAGE : SKSE_PAIR_OK;
}
#else
static SksePairResult capture_file(int source, int dir, int *out) {
    (void)source; (void)dir; (void)out;
    return SKSE_PAIR_UNSUPPORTED;
}
#endif

SksePairResult skse_ess_capture_immutable(int source, int dir, SkseImmutableEss **out) {
    if (!out) return SKSE_PAIR_ARGUMENT;
    *out = NULL;
    struct stat before, after, tmp, captured;
    int flags = fcntl(source, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(source, &before) ||
        !S_ISREG(before.st_mode) || before.st_size <= 0) return SKSE_PAIR_ARGUMENT;
    if ((uint64_t)before.st_size > SKSE_PAIR_MAX_ESS) return SKSE_PAIR_LIMIT;
    if (fstat(dir, &tmp) || !S_ISDIR(tmp.st_mode) || tmp.st_uid != geteuid() ||
        (tmp.st_mode & (S_IRWXG | S_IRWXO))) return SKSE_PAIR_ARGUMENT;
    int fd = -1;
    SksePairResult result = capture_file(source, dir, &fd);
    if (result != SKSE_PAIR_OK) return result;
    if (fstat(source, &after) || fstat(fd, &captured)) result = SKSE_PAIR_STORAGE;
    else if (!skse_ess_same_version(&before, &after) || captured.st_size != before.st_size)
        result = SKSE_PAIR_ESS_CHANGED;
    else if (!S_ISREG(captured.st_mode) || captured.st_nlink != 0) result = SKSE_PAIR_STORAGE;
    if (result == SKSE_PAIR_OK) {
        *out = calloc(1, sizeof **out);
        if (*out) (*out)->fd = fd;
        else result = SKSE_PAIR_STORAGE;
    }
    if (result != SKSE_PAIR_OK) close(fd);
    return result;
}

SksePairResult skse_ess_read_immutable(SkseImmutableEss *snapshot, SkseEssDigest *out) {
    if (!snapshot || !out) return SKSE_PAIR_ARGUMENT;
    if (!snapshot->hashed) {
        SksePairResult result = skse_ess_digest(snapshot->fd, &snapshot->digest);
        if (result != SKSE_PAIR_OK) return result;
        snapshot->hashed = 1;
    }
    *out = snapshot->digest;
    return SKSE_PAIR_OK;
}
void skse_ess_release_immutable(SkseImmutableEss *snapshot) {
    if (snapshot) { close(snapshot->fd); free(snapshot); }
}
