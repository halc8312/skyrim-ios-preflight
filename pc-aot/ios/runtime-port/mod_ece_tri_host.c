/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "mod_ece_tri_host.h"
#include "ece_nose_witness.h"

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
    bool nose;
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

static bool configured_path(uint32_t name, bool relative) {
    static const char expected[] = "meshes/actors/character/character assets/femaleheadchargen.tri";
    if (name < FIRST_MAPPED || name > UINT32_MAX - (sizeof expected - 1)) return false;
    const size_t prefix = relative ? sizeof "meshes/" - 1 : 0;
    for (size_t i = prefix; i < sizeof expected; ++i) {
        unsigned char c = MEM[name + i - prefix];
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

static uint64_t begin_read(const struct CPU *cpu, uint32_t path, uint32_t stream, bool nose) {
    if (winner < 0 || !MEM || !cpu || cpu != rt_current_cpu() ||
        (!configured_path(path, false) && !(nose && configured_path(path, true)))) return 0;
    if (current.ticket) {
        if (nose) return 0;
        // Preserve the original observation if a direct nose read invokes it.
        // A nested legacy read still invalidates a legacy outer observation.
        if (!current.nose) {
            current.source = UNKNOWN;
            skyrim_ece_tri_read_pending = false;
            return 0;
        }
        skyrim_ece_tri_read_pending = false;
    }
    current.owner = cpu;
    current.nose = nose;
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

uint64_t skyrim_ece_tri_app_begin(const struct CPU *cpu, uint32_t path, uint32_t stream) {
    return begin_read(cpu, path, stream, false);
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

static bool finish_read(const struct CPU *cpu, uint64_t ticket, bool nose, unsigned *source) {
    if (!ticket || current.ticket != ticket || current.nose != nose ||
        cpu != current.owner || cpu != rt_current_cpu()) return false;
    *source = current.source;
    current.ticket = 0;
    current.owner = NULL;
    current.stream = 0;
    current.source = UNKNOWN;
    skyrim_ece_tri_read_pending = false;
    return true;
}

void skyrim_ece_tri_app_finish(const struct CPU *cpu, uint64_t ticket, uint8_t parsed) {
    unsigned source;
    if (!finish_read(cpu, ticket, false, &source)) return;
    if (!parsed) source = UNKNOWN;
    const unsigned result = (unsigned)!!parsed * 3u + source;
    const unsigned bit = 1u << result;
    if (atomic_fetch_or_explicit(&reported, bit, memory_order_relaxed) & bit) return;
    const int saved_errno = errno;
    fprintf(stderr, "IOS_ECE_TRI schema=1 source=%u parsed=%u\n", source, (unsigned)!!parsed);
    errno = saved_errno;
}

static _Thread_local const struct CPU *nose_owner;
static _Thread_local bool nose_attempt;
enum { NOSE_ATTEMPT_LIMIT = 64 };
static atomic_uint nose_attempts;
static _Atomic uint64_t nose_reports;

static bool read_nose_field(void *memory, uint32_t address, void *output, size_t size) {
    if (!memory || address < FIRST_MAPPED || (size != 1 && size != 4) ||
        address > UINT32_MAX - (size - 1)) return false;
    memcpy(output, (const uint8_t *)memory + address, size);
    return true;
}

static uint64_t begin_nose_read(void *memory, const struct CPU *cpu, uint32_t path, uint32_t stream) {
    return memory == MEM && cpu == nose_owner ? begin_read(cpu, path, stream, true) : 0;
}

static bool finish_nose_read(void *memory, const struct CPU *cpu, uint64_t receipt) {
    unsigned source;
    if (memory != MEM || cpu != nose_owner || !finish_read(cpu, receipt, true, &source) || source != LOOSE) return false;
    const int saved_errno = errno;
    struct stat stat;
    const bool same = winner >= 0 && !fstat(winner, &stat) && same_file(&stat);
    errno = saved_errno;
    return same;
}

static void report_nose(void *memory, EceNoseWitnessReport result) {
    if (memory != MEM || !nose_attempt) return;
    const unsigned key = (unsigned)result.female_nord << 5 | (unsigned)result.head << 4 |
                         (unsigned)result.geometry << 3 | (unsigned)result.selected_path << 2 |
                         (unsigned)result.fresh_tri << 1 | (unsigned)result.morph_record;
    const uint64_t bit = UINT64_C(1) << key;
    if (atomic_fetch_or_explicit(&nose_reports, bit, memory_order_relaxed) & bit) return;
    const int saved_errno = errno;
    fprintf(stderr, "IOS_ECE_NOSE schema=1 female_nord=%u head=%u geometry=%u path=%u fresh=%u record=%u\n",
            (unsigned)result.female_nord, (unsigned)result.head, (unsigned)result.geometry,
            (unsigned)result.selected_path, (unsigned)result.fresh_tri, (unsigned)result.morph_record);
    errno = saved_errno;
}

static bool claim_nose_attempt(void) {
    unsigned count = atomic_load_explicit(&nose_attempts, memory_order_relaxed);
    while (count < NOSE_ATTEMPT_LIMIT)
        if (atomic_compare_exchange_weak_explicit(&nose_attempts, &count, count + 1,
                                                  memory_order_relaxed, memory_order_relaxed)) return true;
    return false;
}

uint64_t skyrim_ece_nose_app_observe(const struct CPU *cpu, enum SkyrimEceNoseNotice site, uint32_t first,
                                    uint32_t second, uint32_t third, uint32_t fourth, uint64_t ticket) {
    if (!MEM || winner < 0 || !cpu || rt_current_cpu() != cpu) return 0;
    if (site == SkyrimEceNoseMenu) {
        if (nose_owner) {
            if (cpu == nose_owner) ece_nose_menu(first, second); /* Discard a reentrant menu scope. */
            return 0;
        }
        if (atomic_load_explicit(&nose_attempts, memory_order_relaxed) >= NOSE_ATTEMPT_LIMIT ||
            !ece_nose_witness_arm(read_nose_field, begin_nose_read, finish_nose_read, report_nose, MEM)) return 0;
        if (!ece_nose_menu(first, second)) {
            ece_nose_witness_disarm();
            return 0;
        }
        nose_owner = cpu;
        nose_attempt = false;
        return 1;
    }
    if (cpu != nose_owner) return 0;
    switch (site) {
    case SkyrimEceNoseMenuEnd:
        ece_nose_menu_end(true);
        nose_owner = NULL;
        nose_attempt = false;
        break;
    case SkyrimEceNoseBegin:
        // Idle menu frames must neither report nor consume the observation budget.
        if (!second || !third) return 0;
        if (!nose_attempt) nose_attempt = claim_nose_attempt();
        return nose_attempt && ece_nose_begin(first, second, third);
    case SkyrimEceNoseEnd: ece_nose_end(true); break;
    case SkyrimEceNoseFace: if (ticket <= UINT32_MAX) ece_nose_face(first, second, third, fourth, (uint32_t)ticket); break;
    case SkyrimEceNoseHead: return ece_nose_head(first, second, third, fourth);
    case SkyrimEceNoseHeadEnd: ece_nose_head_end(true); break;
    case SkyrimEceNoseLookup: return ece_nose_lookup(first);
    case SkyrimEceNoseLookupEnd: ece_nose_lookup_end(true, first); break;
    case SkyrimEceNoseParse: return ece_nose_parse(cpu, first, second);
    case SkyrimEceNoseParsed: return ece_nose_parsed(cpu, ticket, first != 0);
    case SkyrimEceNoseConstructed: ece_nose_constructed(true, first); break;
    case SkyrimEceNoseRetiring: ece_nose_retiring(first); break;
    case SkyrimEceNoseMorph: return ece_nose_morph(first, second);
    case SkyrimEceNoseMorphRecord: ece_nose_morph_record(true, first, second != 0); break;
    case SkyrimEceNoseMenu: break;
    }
    return 0;
}
