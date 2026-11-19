/* SPDX-License-Identifier: GPL-3.0-only */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static int classic_test_fsync(int fd) {
    if (getenv("CLASSIC_TEST_SYNC_ERROR")) { errno = EIO; return -1; }
    return fsync(fd);
}
static int classic_test_publish(const char *source, const char *destination) {
    if (getenv("CLASSIC_TEST_KILL_BEFORE_PUBLISH")) raise(SIGKILL);
    if (getenv("CLASSIC_TEST_LINK_ERROR")) { errno = EOPNOTSUPP; return -1; }
    return link(source, destination);
}

/* Substitute only these two calls in the real editor, after libc declarations. */
#define fsync classic_test_fsync
#define link classic_test_publish
#include "../../ios/runtime-port/classic_cosave.c"
