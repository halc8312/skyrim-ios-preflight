/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "roundtrip.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t game = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int original = -1, destination = -1;
static _Thread_local bool clone_unavailable, attempted_clone;
static _Atomic size_t hashed;

void roundtrip_enter_game(void) { assert(!pthread_mutex_lock(&game)); }
void roundtrip_leave_game(void) { assert(!pthread_mutex_unlock(&game)); }

static void assert_unlocked(void) {
    assert(!pthread_mutex_trylock(&game));
    assert(!pthread_mutex_unlock(&game));
}

void roundtrip_expect_clone(int source, bool unavailable) {
    assert(original == -1 && destination == -1 && !attempted_clone);
    original = source;
    clone_unavailable = unavailable;
}

void roundtrip_finish_clone(bool attempted) {
    assert(attempted == attempted_clone);
    original = destination = -1;
    attempted_clone = false;
}

/* Only the hosted immutable_ess object uses these two substitutes. They
 * emulate clone by copying the exact verified authored source after unlock. */
int roundtrip_openat(int directory, const char *name, int flags, int mode) {
    struct stat info;
    assert(original >= 0 && destination == -1 && !strcmp(name, "."));
    assert(flags == (O_TMPFILE | O_EXCL | O_RDWR | O_CLOEXEC) && mode == 0600);
    assert(!fstat(directory, &info) && S_ISDIR(info.st_mode) && !(info.st_mode & (S_IRWXG | S_IRWXO)));
    destination = memfd_create("authored-immutable-save", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    return destination;
}

int roundtrip_ioctl(int target, unsigned long request, int source) {
    assert_unlocked();
    assert(request == FICLONE && target == destination && target >= 0 && !attempted_clone);
    assert(source == original);
    attempted_clone = true;
    if (clone_unavailable) {
        errno = EOPNOTSUPP;
        return -1;
    }
    struct stat info;
    char bytes[128];
    const int mode = fcntl(source, F_GETFL);
    assert(mode >= 0 && (mode & O_ACCMODE) == O_RDONLY && !fstat(source, &info));
    assert(S_ISREG(info.st_mode) && info.st_size > 0 && (uint64_t)info.st_size <= sizeof bytes);
    assert(pread(source, bytes, (size_t)info.st_size, 0) == info.st_size);
    assert(write(target, bytes, (size_t)info.st_size) == info.st_size);
    const int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL;
    assert(!fcntl(target, F_ADD_SEALS, seals));
    return 0;
}

ssize_t roundtrip_digest_pread(int fd, void *out, size_t count, off_t offset) {
    assert_unlocked();
    const int seals = fcntl(fd, F_GET_SEALS);
    const int required = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE;
    assert(seals >= 0 && (seals & required) == required);
    const ssize_t read = pread(fd, out, count, offset);
    if (read > 0) atomic_fetch_add_explicit(&hashed, (size_t)read, memory_order_relaxed);
    return read;
}

size_t roundtrip_digest_bytes(void) { return atomic_load_explicit(&hashed, memory_order_relaxed); }
