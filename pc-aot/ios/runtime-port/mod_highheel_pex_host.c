/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "mod_highheel_pex_host.h"
#include "classic_vm_epoch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern uint8_t *MEM;
extern struct CPU *rt_current_cpu(void);

enum {
    FIRST_MAPPED = 0x10000, SCRIPT_LOADER = 0x1149B98, RESOURCE_STORE = 0x10EA8C8,
    STORE_STREAM = 0x18, STORE_OPEN = 0x20, LOOSE_STREAM = 0x110CD68,
    LOOSE_HANDLE = 0x1c, LOOSE_BACKING_STREAM = 0x30, LOOSE_READ_RETURN = 0xA580E2,
    PEX_BYTES = 5875
};
static SkyrimHighHeelPexDigest digest_bytes;
static atomic_uint reported;
static _Atomic(uint64_t) last_linked_epoch;
static _Thread_local struct {
    const struct CPU *owner;
    SkyrimVmScope vm;
    uint32_t store;
    int file;
    struct stat identity;
} attempt;
_Thread_local bool skyrim_highheel_pex_read_pending;

void skyrim_highheel_pex_configure(SkyrimHighHeelPexDigest digest) { digest_bytes = digest; }

static bool guest_object(uint32_t object, unsigned last) {
    return object >= FIRST_MAPPED && !(object & 3) && object <= UINT32_MAX - last;
}

static uint32_t word(uint32_t address) {
    uint32_t value;
    memcpy(&value, MEM + address, sizeof value);
    return value;
}

static bool is_effect_name(uint32_t name) {
    static const char expected[] = "hdthighheeleffect";
    if (name < FIRST_MAPPED || name > UINT32_MAX - (sizeof expected - 1)) return false;
    for (size_t i = 0; i < sizeof expected; ++i) {
        unsigned char c = MEM[name + i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)expected[i]) return false;
    }
    return true;
}

static bool current_vm(SkyrimVmScope expected) {
    SkyrimVmScope now;
    return skyrim_vm_scope_current(&now) && now.epoch == expected.epoch && now.registry == expected.registry;
}

static bool begin(const struct CPU *cpu, uint32_t name, uint32_t loader) {
    if (!digest_bytes || attempt.owner || !MEM || !cpu || cpu != rt_current_cpu() || !is_effect_name(name) ||
        !guest_object(loader, 0x0b) || word(loader) != SCRIPT_LOADER) return false;
    const uint32_t store = word(loader + 8);
    SkyrimVmScope vm;
    if (!guest_object(store, STORE_OPEN) || word(store) != RESOURCE_STORE || MEM[store + STORE_OPEN] ||
        word(store + STORE_STREAM) || !skyrim_vm_scope_current(&vm)) return false;
    attempt.owner = cpu;
    attempt.vm = vm;
    attempt.store = store;
    attempt.file = -1;
    skyrim_highheel_pex_read_pending = true;
    return true;
}

static bool metadata_matches(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
    const struct timespec at = a->st_mtimespec, bt = b->st_mtimespec;
    const struct timespec ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
    const struct timespec at = a->st_mtim, bt = b->st_mtim;
    const struct timespec ac = a->st_ctim, bc = b->st_ctim;
#endif
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) && a->st_size == PEX_BYTES && b->st_size == PEX_BYTES &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino && at.tv_sec == bt.tv_sec && at.tv_nsec == bt.tv_nsec &&
           ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

static bool verify_pex(int fd, struct stat *identity) {
    static const uint8_t expected[] = {
        0x01, 0xe4, 0x81, 0x6a, 0x1e, 0xa6, 0xc5, 0xbc, 0xb6, 0xa4, 0xc3, 0xbf, 0x62, 0xc9, 0xcd, 0xca,
        0x65, 0x74, 0x60, 0x0d, 0xdc, 0x44, 0x1b, 0x95, 0x34, 0x1b, 0x49, 0x42, 0xdf, 0x3b, 0xdf, 0x03
    };
    if (fstat(fd, identity) || !S_ISREG(identity->st_mode) || identity->st_size != PEX_BYTES) return false;
    uint8_t bytes[PEX_BYTES], digest[sizeof expected];
    size_t done = 0;
    while (done < sizeof bytes) {
        const ssize_t n = pread(fd, bytes + done, sizeof bytes - done, (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    struct stat after;
    return !fstat(fd, &after) && metadata_matches(identity, &after) &&
        digest_bytes(bytes, sizeof bytes, digest) && !memcmp(digest, expected, sizeof expected);
}

void skyrim_highheel_pex_read(const struct CPU *cpu, uint32_t caller, uint32_t handle, int fd, ssize_t count) {
    if (!skyrim_highheel_pex_read_pending || cpu != attempt.owner || cpu != rt_current_cpu() ||
        caller != LOOSE_READ_RETURN || !MEM[attempt.store + STORE_OPEN] || count <= 0 || fd < 0) return;
    const uint32_t stream = word(attempt.store + STORE_STREAM);
    if (!guest_object(stream, LOOSE_BACKING_STREAM + 3) || word(stream) != LOOSE_STREAM ||
        word(stream + LOOSE_BACKING_STREAM) || word(stream + LOOSE_HANDLE) != handle) return;
    skyrim_highheel_pex_read_pending = false;
    const int saved_errno = errno;
    const int retained = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (retained >= 0) {
        if (verify_pex(retained, &attempt.identity)) attempt.file = retained;
        else close(retained);
    }
    errno = saved_errno;
}

static void report(unsigned phase, bool passed) {
    const unsigned bit = 1u << (phase * 2u + (unsigned)passed);
    if (atomic_fetch_or_explicit(&reported, bit, memory_order_relaxed) & bit) return;
    const int saved_errno = errno;
    fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_PEX schema=1 phase=%u verified=%u\n", phase, (unsigned)passed);
    errno = saved_errno;
}

static uint64_t end(const struct CPU *cpu, bool parsed) {
    if (!attempt.owner || cpu != attempt.owner || cpu != rt_current_cpu()) return 0;
    skyrim_highheel_pex_read_pending = false;
    const int saved_errno = errno;
    struct stat after;
    const bool verified = parsed && attempt.file >= 0 && !fstat(attempt.file, &after) &&
                          metadata_matches(&attempt.identity, &after) && current_vm(attempt.vm);
    const uint64_t epoch = attempt.vm.epoch;
    if (attempt.file >= 0) close(attempt.file);
    attempt.owner = NULL;
    report(0, verified);
    errno = saved_errno;
    return verified ? epoch : 0;
}

static void linked(const struct CPU *cpu, uint64_t epoch, bool passed) {
    SkyrimVmScope vm;
    if (!cpu || cpu != rt_current_cpu() || !epoch || !skyrim_vm_scope_current(&vm) || epoch != vm.epoch) return;
    if (passed) {
        uint64_t prior = atomic_load_explicit(&last_linked_epoch, memory_order_relaxed);
        while (prior < epoch && !atomic_compare_exchange_weak_explicit(&last_linked_epoch, &prior, epoch,
                                                                        memory_order_release, memory_order_relaxed)) {}
    }
    report(1, passed);
}

bool skyrim_highheel_pex_linked_in(uint64_t epoch) {
    return epoch && atomic_load_explicit(&last_linked_epoch, memory_order_acquire) == epoch;
}

void skyrim_highheel_pex_fill(PcGuestModHooks *table) {
    table->highheel_pex = (PcGuestModHighHeelPexHooks){begin, end, linked};
}
