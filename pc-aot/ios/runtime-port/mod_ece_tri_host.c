/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "mod_ece_tri_host.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern uint8_t *MEM;
extern struct CPU *rt_current_cpu(void);
static bool same_file(const struct stat *actual);

enum {
    FIRST_MAPPED = 0x10000, LOOSE_STREAM = 0x110CD68,
    ARCHIVE_STREAM = 0x110CBB4, ARCHIVE_ENTRY_STREAM = 0x110CBE4,
    LOOSE_FILE_HANDLE = 0x1C, LOOSE_READ_RETURN = 0xA58152,
    UNKNOWN = 0, LOOSE = 1, BSA = 2
};

static bool allowed;
static int winner = -1;
static struct stat identity;
static atomic_uint reported;
static _Thread_local struct {
    const struct CPU *owner;
    uint64_t next, ticket;
    uint32_t stream;
    unsigned source;
} current;
_Thread_local bool skyrim_ece_tri_read_pending;

void skyrim_ece_tri_allow_input(const char *sha256, bool detailed) {
    allowed = detailed && sha256 &&
        !strcmp(sha256, "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605");
}

bool skyrim_ece_tri_configure(int data, SkyrimEceProfileDigest digest) {
    if (!allowed || winner >= 0) return false;
    int fd = -1;
    if (skyrim_ece_profile_open_female_nord(data, digest, &fd, &identity) != SKYRIM_ECE_PROFILE_MATCH)
        return false;
    struct stat after;
    if (fstat(fd, &after) || !same_file(&after)) {
        close(fd);
        return false;
    }
    winner = fd;
    return true;
}

void skyrim_ece_tri_retire(void) {
    if (winner >= 0) close(winner);
    winner = -1;
}

static bool configured_path(uint32_t name) {
    static const char expected[] = "meshes/actors/character/character assets/femaleheadchargen.tri";
    if (name < FIRST_MAPPED || name > UINT32_MAX - (sizeof expected - 1)) return false;
    for (size_t i = 0; i < sizeof expected; ++i) {
        unsigned char c = MEM[name + i];
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)expected[i]) return false;
    }
    return true;
}

static uint32_t word(uint32_t address) {
    uint32_t value;
    memcpy(&value, MEM + address, sizeof value);
    return value;
}

uint64_t skyrim_ece_tri_app_begin(const struct CPU *cpu, uint32_t path, uint32_t stream) {
    if (winner < 0 || !MEM || !cpu || cpu != rt_current_cpu() || !configured_path(path)) return 0;
    if (current.ticket) {
        // A nested load of the same path cannot certify the outer result.
        current.source = UNKNOWN;
        skyrim_ece_tri_read_pending = false;
        return 0;
    }
    current.owner = cpu;
    current.ticket = ++current.next;
    if (!current.ticket) current.ticket = ++current.next;
    current.stream = stream;
    current.source = UNKNOWN;
    if (stream >= FIRST_MAPPED && stream <= UINT32_MAX - LOOSE_FILE_HANDLE - 3 && !(stream & 3)) {
        const uint32_t type = word(stream);
        skyrim_ece_tri_read_pending = type == LOOSE_STREAM;
        if (type == ARCHIVE_STREAM || type == ARCHIVE_ENTRY_STREAM) current.source = BSA;
    }
    return current.ticket;
}

static bool same_file(const struct stat *actual) {
#ifdef __APPLE__
    const struct timespec mtime = actual->st_mtimespec, expected = identity.st_mtimespec;
    const struct timespec ctime = actual->st_ctimespec, expected_change = identity.st_ctimespec;
#else
    const struct timespec mtime = actual->st_mtim, expected = identity.st_mtim;
    const struct timespec ctime = actual->st_ctim, expected_change = identity.st_ctim;
#endif
    return S_ISREG(actual->st_mode) && actual->st_dev == identity.st_dev && actual->st_ino == identity.st_ino &&
           actual->st_size == identity.st_size && mtime.tv_sec == expected.tv_sec && mtime.tv_nsec == expected.tv_nsec &&
           ctime.tv_sec == expected_change.tv_sec && ctime.tv_nsec == expected_change.tv_nsec;
}

void skyrim_ece_tri_app_read(const struct CPU *cpu, uint32_t caller, uint32_t handle, int fd, ssize_t bytes) {
    if (!skyrim_ece_tri_read_pending || current.owner != cpu || cpu != rt_current_cpu() ||
        caller != LOOSE_READ_RETURN || bytes <= 0 || fd < 0 ||
        word(current.stream) != LOOSE_STREAM || handle != word(current.stream + LOOSE_FILE_HANDLE)) return;
    const int saved_errno = errno;
    struct stat actual;
    current.source = fstat(fd, &actual) == 0 && same_file(&actual) ? LOOSE : UNKNOWN;
    skyrim_ece_tri_read_pending = false;
    errno = saved_errno;
}

void skyrim_ece_tri_app_finish(const struct CPU *cpu, uint64_t ticket, uint8_t parsed) {
    if (!ticket || current.ticket != ticket || cpu != current.owner || cpu != rt_current_cpu()) return;
    const unsigned source = parsed ? current.source : UNKNOWN;
    current.ticket = 0;
    current.owner = NULL;
    current.stream = 0;
    current.source = UNKNOWN;
    skyrim_ece_tri_read_pending = false;
    const unsigned result = (unsigned)!!parsed * 3u + source;
    const unsigned bit = 1u << result;
    if (atomic_fetch_or_explicit(&reported, bit, memory_order_relaxed) & bit) return;
    const int saved_errno = errno;
    fprintf(stderr, "IOS_ECE_TRI schema=1 source=%u parsed=%u\n", source, (unsigned)!!parsed);
    errno = saved_errno;
}
