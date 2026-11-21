/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "cosave_fixture.h"
#include "event_bridge.h"
#include "rt_table.h"
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
#include "roundtrip.h"
#include "records.h"
#include "save_paths.h"
#include <pthread.h>
#endif

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { VM = 0x3b000 };
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
typedef enum { MATCHED, MISSING, REPLACED, PRELOADED } RoundtripLoad;
static RoundtripLoad load_mode;
static const char *base_bytes;
static struct stat opened_input;
#else
static const char base_bytes[] = "authored input";
static int source = -1;
static size_t expected_size;
static void *expected_bytes;
#endif
static SkseEventBridge *bridge;
static SkseGameEvent event = {
    .scope = {.launch = {1}, .profile = {2}},
    .saved = {.profile = {2}, .slot = {3}, .event = {4}},
};
static SkseLoadInput *pending;
static int scratch = -1, loader = -1;
static size_t before_count, read_count;
static enum ClassicCosaveCase test_case;
static unsigned vm_reverts, vm_loads;
static uint32_t vm_stream, vm_frame;
static SkyrimLifecycleScope lifecycle;

void f_8D6990(CPU *);
void f_8D69C0(CPU *);

#if defined(SKYRIM_TEST_PROJECT_FORM)
void classic_test_projection_after_load(const SkseGameEvent *, const SkseEventTrace *, SkseBridgeResult);
#endif

#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
void classic_cosave_roundtrip_configure(const char *expected) {
    if (!strcmp(expected, "matched")) load_mode = MATCHED;
    else if (!strcmp(expected, "missing")) load_mode = MISSING;
    else if (!strcmp(expected, "replaced")) load_mode = REPLACED;
    else if (!strcmp(expected, "preloaded")) load_mode = PRELOADED;
    else assert(0 && "Unknown roundtrip load case");
    base_bytes = load_mode == REPLACED ? ROUNDTRIP_REPLACEMENT : ROUNDTRIP_ORIGINAL;
    event.scope.launch[0] = 5; /* Separate launch, same generation key. */
}

bool classic_cosave_roundtrip_cached(void) { return load_mode == PRELOADED; }

bool classic_cosave_roundtrip_same_input(int fd) {
    struct stat held;
    return pending && !fstat(fd, &held) && skse_save_same_version(&opened_input, &held);
}

static void *try_other_load_scope(void *unused) {
    (void)unused;
    SkyrimLifecycleScope scope;
    assert(skyrim_lifecycle_scope_open(&scope));
    const SkyrimLifecycleCall other = skyrim_lifecycle_begin(SkyrimLoadSlot);
    SkseGameEvent rejected;
    assert(skse_bridge_bind_load_slot(pending, &rejected) == SKSE_BRIDGE_SCOPE && !rejected.ticket);
    skyrim_lifecycle_end(other, SkyrimReturnedTrue);
    assert(skyrim_lifecycle_scope_close(&scope));
    return NULL;
}

void classic_cosave_roundtrip_bind_slot(void) {
    if (load_mode == PRELOADED) return;
    SkseGameEvent rejected;
    assert(skse_bridge_bind_load_slot(pending, &event) == SKSE_BRIDGE_OK);
    assert(event.ticket == before_count + 3);
    assert(skse_bridge_bind_load_slot(pending, &rejected) == SKSE_BRIDGE_STATE && !rejected.ticket);
}

static int verify_record(void *context, const SksePairRecord *record) {
    unsigned *visited = context;
    assert(!*visited && record->type == ROUNDTRIP_RECORD && record->version == ROUNDTRIP_RECORD_VERSION);
    assert(record->length == sizeof ROUNDTRIP_RECORD_DATA && !memcmp(record->data, ROUNDTRIP_RECORD_DATA, record->length));
    ++*visited;
    return 1;
}

static void verify_sidecar(const SksePairImage *image) {
    unsigned visited = 0;
    assert(skse_records_validate(image->data, image->size) == SKSE_PAIR_OK);
    assert(skse_records_visit(image, ROUNDTRIP_OWNER, verify_record, &visited) == SKSE_PAIR_OK && visited == 1);
}
#endif

void classic_cosave_fixture_init(const char *directory, const char *selected) {
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    struct stat input;
    assert(!lstat(selected, &input) && S_ISREG(input.st_mode) && input.st_size > 0 && base_bytes);
#else
    source = memfd_create("selected-ess", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(source >= 0 && write(source, base_bytes, sizeof base_bytes - 1) == sizeof base_bytes - 1);
    assert(fcntl(source, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK) == 0);
    char source_ref[64];
    int length = snprintf(source_ref, sizeof source_ref, "/proc/self/fd/%d", source);
    assert(length > 0 && (size_t)length < sizeof source_ref && !symlink(source_ref, selected));
#endif
    scratch = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(scratch >= 0);
    SksePairResult detail;
    assert(skse_bridge_open(directory, &event.scope, &bridge, &detail) == SKSE_BRIDGE_OK);

#if !defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    char store_path[CLASSIC_FRESH_PATH_BYTES];
    length = snprintf(store_path, sizeof store_path, "%s/text-en-voice-en", directory);
    assert(length > 0 && (size_t)length < sizeof store_path);
    SksePairStore *store = NULL;
    assert(skse_pair_open(store_path, &store) == SKSE_PAIR_OK);
    const int fd = open(selected, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    const SksePairGeneration generation = {event.saved, fd};
    const SksePairRecord record = {0x1234, 1, 4, "test"};
    SksePairStage *stage = NULL;
    assert(skse_pair_begin(store, &event.saved, NULL, &stage) == SKSE_PAIR_OK);
    assert(skse_pair_replace(stage, 0x0987654, &record, 1) == SKSE_PAIR_OK);
    assert(skse_pair_pin(stage, &generation) == SKSE_PAIR_OK);
    assert(skse_pair_commit(store, stage, &generation) == SKSE_PAIR_OK);
    assert(skse_pair_export(store, &generation, &expected_bytes, &expected_size) == SKSE_PAIR_OK);
    assert(expected_size > 20 && !close(fd));
    skse_pair_discard(stage);
    skse_pair_close(store);
#endif
}

void classic_cosave_fixture_begin(enum ClassicCosaveCase mode) {
    assert(!pending && loader < 0);
    assert(skyrim_lifecycle_scope_open(&lifecycle));
    test_case = mode;
    vm_reverts = vm_loads = 0;
    read_count = 0;
}

int classic_cosave_fixture_load_fd(int original) {
    assert(!pending && loader < 0 && !read_count);
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    uint64_t dropped = 0;
    before_count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &dropped);
    assert(!dropped && before_count < SkyrimLifecycleCapacity);
    assert(lseek(original, 7, SEEK_SET) == 7);
    SksePairResult detail;
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    assert(load_mode != PRELOADED && roundtrip_digest_bytes() == 0);
    roundtrip_expect_clone(original, false);
    assert(skse_bridge_capture_named_load(bridge, skyrim_lifecycle_scope_current, scratch, "Fixture.ess",
                                          original, scratch, &pending, &loader, &detail)
           == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK && pending && loader >= 0);
    SkseGameEvent rejected;
    assert(skse_bridge_bind_load_slot(pending, &rejected) == SKSE_BRIDGE_EVENT && !rejected.ticket);
    pthread_t other;
    assert(!pthread_create(&other, NULL, try_other_load_scope, NULL) && !pthread_join(other, NULL));
#else
    /* Exercise the original explicit-ticket path, including a shifted ticket. */
    event.ticket = (uint32_t)before_count + 1;
    assert(skse_bridge_prepare_load_input(bridge, &event, original, scratch, &pending, &loader, &detail)
           == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK && pending && loader >= 0);
#endif
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    roundtrip_finish_clone(true);
    assert(!fstat(loader, &opened_input));
#endif
    assert(loader != original && lseek(loader, 0, SEEK_CUR) == 0 && lseek(original, 0, SEEK_CUR) == 7);
    const int flags = fcntl(loader, F_GETFL), seals = fcntl(loader, F_GET_SEALS);
    const int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    assert(flags >= 0 && (flags & O_ACCMODE) == O_RDONLY && seals >= 0 && (seals & required) == required);
    assert(!close(original));
    return loader;
}

size_t classic_cosave_fixture_read(int fd, bool in_slot) {
    char byte;
    assert(fd == loader && read_count == (size_t)in_slot);
    assert(lseek(fd, 0, SEEK_CUR) == (off_t)read_count);
    const ssize_t actual = read(fd, &byte, sizeof byte);
    assert(actual == 1 && byte == base_bytes[read_count]);
    ++read_count;
    return (size_t)actual;
}

void classic_cosave_fixture_after_metadata(void) {
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    if (load_mode == PRELOADED) return;
    SkyrimLifecycleCall extra = skyrim_lifecycle_begin(SkyrimRegisterPapyrus);
    SkseGameEvent rejected;
    assert(skse_bridge_bind_load_slot(pending, &rejected) == SKSE_BRIDGE_EVENT && !rejected.ticket);
    skyrim_lifecycle_end(extra, SkyrimNoResult);
#else
    if (test_case == COSAVE_TICKET_SHIFT) {
        SkyrimLifecycleCall extra = skyrim_lifecycle_begin(SkyrimRegisterPapyrus);
        skyrim_lifecycle_end(extra, SkyrimNoResult);
    }
#endif
}

void f_C398A0(CPU *cpu) {
    assert(cpu->r[1] == VM + 0x150 && M32(cpu->r[4]) == 0x8d69a5);
    cpu->r[4] += 12;
}

void f_8D5DF0(CPU *cpu) {
    assert(cpu->r[1] == VM && M32(cpu->r[4]) == 0x8d69ac && !vm_reverts && !vm_loads);
    ++vm_reverts;
    cpu->r[4] += 4;
}

void f_401710(CPU *cpu) {
    assert(cpu->r[1] == VM + 0x238 && M32(cpu->r[4]) == 0x8b9b23);
    cpu->r[4] += 8;
}

void f_8B9AA0(CPU *cpu) {
    assert(cpu->r[1] == VM + 0x274 && M32(cpu->r[4]) == 0x8b9b3c);
    cpu->r[4] += 8;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cpu;
    (void)target;
    (void)cache;
    assert(0 && "Unexpected refcount transition");
}

void f_8D3DB0(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(cpu->r[1] == VM && M32(stack) == 0x8d6b7e && !vm_loads);
    assert(M32(stack + 4) == vm_frame + 0x14 && M32(stack + 8) == vm_stream);
    ++vm_loads;
    cpu->r[0] = test_case == COSAVE_VM_REJECT ? 0x9a1100 : 0x9a1101;
    cpu->r[4] += 12;
}

void classic_cosave_fixture_vm(const CPU *cpu, uint32_t stream) {
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    roundtrip_enter_game();
#endif
    CPU vm = *cpu;
    vm_frame = cpu->r[4] - 0x100;
    vm_stream = stream;
    vm.r[1] = VM;
    vm.r[4] = vm_frame;
    if (test_case != COSAVE_NO_REVERT) {
        M32(VM + 0x23c) = 2;
        f_8D6990(&vm);
        assert(vm.r[4] == vm_frame + 4 && M32(VM + 0x23c) == 1);
    }
    if (test_case != COSAVE_NO_VM) {
        vm.r[4] = vm_frame;
        vm.r[6] = VM;
        vm.r[7] = stream;
        f_8D69C0(&vm);
        assert(vm.r[4] == vm_frame && (uint8_t)vm.r[0] == (test_case != COSAVE_VM_REJECT));
    }
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    roundtrip_leave_game();
#endif
}

void classic_cosave_fixture_finish(ClassicFreshReport report, const char *other) {
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    if (load_mode == PRELOADED) {
        assert(report.result == CLASSIC_FRESH_CACHED && report.slot_ok && report.outer_ok);
        assert(!pending && loader < 0 && !read_count && roundtrip_digest_bytes() == 0);
        assert(vm_reverts == 1 && vm_loads == 1);
        assert(skyrim_lifecycle_scope_close(&lifecycle));
        return;
    }
#endif
    assert(pending && loader >= 0 && read_count == (test_case == COSAVE_NO_SLOT_READ ? 1u : 2u));
    assert(vm_reverts == (test_case != COSAVE_NO_REVERT) && vm_loads == (test_case != COSAVE_NO_VM));
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    SkseEventTrace trace = {.rows = rows};
    memcpy(trace.launch, event.scope.launch, sizeof trace.launch);
    trace.count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &trace.dropped);
#if !defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    assert(skyrim_lifecycle_scope_close(&lifecycle));
#endif
    uint32_t actual_ticket = 0;
    bool returned = false;
    for (size_t i = before_count; i < trace.count; ++i) {
        const SkyrimLifecycleRow *row = &rows[i];
        if (row->site != SkyrimLoadSlot) continue;
        assert(!row->parent);
        if (row->returning) {
            assert(actual_ticket && actual_ticket == row->ticket && !returned && report.slot_returned);
            assert(row->outcome == (report.slot_ok ? SkyrimReturnedTrue : SkyrimReturnedFalse));
            assert(report.slot_ok == (test_case != COSAVE_BASE_REJECT));
            returned = true;
        } else {
            assert(!actual_ticket);
            actual_ticket = row->ticket;
        }
    }
    assert(returned && (actual_ticket != event.ticket) == (test_case == COSAVE_TICKET_SHIFT));
    SkseBaseEvidence proof = {event, SKSE_BASE_FAILED, -1};
    proof.event.ticket = actual_ticket;
    if (report.result == CLASSIC_FRESH_READY) {
        assert(lseek(report.ess_fd, 0, SEEK_CUR) == (off_t)read_count);
        proof.outcome = SKSE_BASE_LOADED;
        proof.ess_fd = report.ess_fd;
    }
    if (test_case == COSAVE_WRONG_FD) {
        proof.ess_fd = open(other, O_RDONLY | O_CLOEXEC);
        assert(proof.ess_fd >= 0);
    }
#if !defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    static const SkseBridgeResult results[] = {
        [COSAVE_ACCEPT] = SKSE_BRIDGE_OK,
        [COSAVE_VM_REJECT] = SKSE_BRIDGE_REJECTED,
        [COSAVE_NO_REVERT] = SKSE_BRIDGE_TRACE,
        [COSAVE_NO_VM] = SKSE_BRIDGE_TRACE,
        [COSAVE_TICKET_SHIFT] = SKSE_BRIDGE_EVENT,
        [COSAVE_WRONG_FD] = SKSE_BRIDGE_PAIR,
        [COSAVE_BASE_REJECT] = SKSE_BRIDGE_REJECTED,
        [COSAVE_NO_SLOT_READ] = SKSE_BRIDGE_REJECTED,
    };
#endif
    void *bytes = NULL;
    size_t size = 0;
    SksePairResult detail;
    const SkseBridgeResult result = skse_bridge_finish_load_input(&pending, &trace, &proof, &bytes, &size, &detail);
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    assert(skyrim_lifecycle_scope_close(&lifecycle));
    assert(!pending && report.result == CLASSIC_FRESH_READY);
    if (load_mode == MATCHED) {
        assert(result == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK && bytes);
        const SksePairImage image = {bytes, size};
        verify_sidecar(&image);
        assert(roundtrip_digest_bytes() == sizeof ROUNDTRIP_ORIGINAL - 1);
    } else {
        assert(result == SKSE_BRIDGE_PAIR && !bytes && !size);
        assert(detail == SKSE_PAIR_MISSING);
    }
#if defined(SKYRIM_TEST_PROJECT_FORM)
    classic_test_projection_after_load(&proof.event, &trace, result);
#endif
#else
    assert(result == results[test_case] && !pending);
    if (test_case == COSAVE_ACCEPT) {
        assert(detail == SKSE_PAIR_OK && size == expected_size && !memcmp(bytes, expected_bytes, size));
    } else {
        assert(!bytes && !size);
    }
    if (test_case == COSAVE_WRONG_FD) {
        assert(detail == SKSE_PAIR_WRONG_GENERATION && !close(proof.ess_fd));
    }
#endif
    free(bytes);
    loader = -1;
}

void classic_cosave_fixture_close(void) {
    assert(!pending && loader < 0);
#if !defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    free(expected_bytes);
    assert(!close(source));
#endif
    skse_bridge_close(bridge);
    assert(!close(scratch));
}
