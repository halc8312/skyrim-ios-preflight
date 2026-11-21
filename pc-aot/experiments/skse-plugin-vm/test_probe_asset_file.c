/* SPDX-License-Identifier: GPL-3.0-only */
/* Fault-injection harness for the read-only fixture. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "probe_asset_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *mode;
static int rootfd;
static unsigned reads;

static void must(bool ok) {
    if (!ok) {
        perror("asset test fixture");
        exit(3);
    }
}

void *skse_asset_fixture_malloc(size_t n) {
    return !strcmp(mode, "oom") ? NULL : malloc(n);
}

ssize_t skse_asset_fixture_read(int fd, void *bytes, size_t n) {
    ++reads;
    if (reads == 1) {
        if (!strcmp(mode, "interrupt")) { errno = EINTR; return -1; }
        if (!strcmp(mode, "error")) { errno = EIO; return -1; }
        if (!strcmp(mode, "short-eof")) return 0;
        if (!strcmp(mode, "move-dir")) {
            must(renameat(rootfd, "nested", rootfd, "moved") == 0);
            must(mkdirat(rootfd, "nested", 0700) == 0);
        }
        if (!strcmp(mode, "replace")) {
            must(renameat(rootfd, "replacement", rootfd, "sample") == 0);
        }
        if (!strcmp(mode, "modify")) {
            int out = openat(rootfd, "sample", O_WRONLY);
            must(out >= 0);
            must(pwrite(out, "!", 1, 0) == 1);
            struct timespec stamp[2] = {{1600000000, 0}, {1600000000, 0}};
            must(futimens(out, stamp) == 0);
            must(close(out) == 0);
        }
    }
    if (!strcmp(mode, "extra") && reads == 2) { *(uint8_t *)bytes = 42; return 1; }
    if (!strcmp(mode, "short") && n > 2) n = 2;
    return read(fd, bytes, n);
}

/* Place the two seams after libc's declarations. Otherwise GNU fortified
 * inline read() may resolve to the original function even in the test. */
#define read skse_asset_fixture_read
#define malloc skse_asset_fixture_malloc
#include "probe_asset_file.c"
#undef read
#undef malloc

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    mode = argv[4];
    rootfd = open(argv[1], O_RDONLY | O_CLOEXEC);
    must(rootfd >= 0);
    /* The adapter must not take ownership, write, or move the directory offset. */
    const off_t before = lseek(rootfd, 0, SEEK_CUR);
    if (!strcmp(mode, "attributes")) {
        uint32_t attributes = UINT32_MAX;
        uint32_t status = skse_probe_asset_attributes(&rootfd, argv[2], &attributes);
        must(lseek(rootfd, 0, SEEK_CUR) == before && close(rootfd) == 0);
        printf("%u|%u|%u\n", status, attributes, reads);
        return 0;
    }
    uint8_t sentinel = 0;
    uint8_t *result = &sentinel;
    size_t length = SIZE_MAX;
    int invalid = -1;
    void *context = !strcmp(mode, "no-context") ? NULL : !strcmp(mode, "bad-fd") ? &invalid : &rootfd;
    uint32_t (*snapshot)(void *, const char *, size_t, uint8_t **, size_t *) =
        !strcmp(mode, "unique") ? skse_probe_asset_read_unique : skse_probe_asset_read;
    uint32_t status = snapshot(context, !strcmp(mode, "no-path") ? NULL : argv[2],
                                (size_t)strtoull(argv[3], NULL, 10),
                                !strcmp(mode, "no-bytes") ? NULL : &result,
                                !strcmp(mode, "no-length") ? NULL : &length);
    if (!status && !strcmp(mode, "after-success-edit")) {
        int out = openat(rootfd, argv[2], O_WRONLY | O_TRUNC);
        must(out >= 0 && write(out, "later", 5) == 5 && close(out) == 0);
    }
    must(fcntl(rootfd, F_GETFD) >= 0 && lseek(rootfd, 0, SEEK_CUR) == before);
    must(close(rootfd) == 0);
    if (status) {
        if (strcmp(mode, "no-bytes")) must(!result);
        if (strcmp(mode, "no-length")) must(!length);
        printf("%u|0|%u|\n", status, reads);
    } else {
        must(result && result != &sentinel);
        printf("0|%zu|%u|", length, reads);
        for (size_t i = 0; i < length; ++i) printf("%02x", result[i]);
        putchar('\n');
        free(result);
    }
    return 0;
}
