/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__linux__)
#define _GNU_SOURCE 1
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "interpreted_records.h"
#include "fixtures/owned_storage.h"
#include "x86_skse_records.h"
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif

enum { Owner = 0x1ced1591u, Otherwise = 0x00feedaa, WorkLimit = 10000 };
static const unsigned char opaque[] = {0xff, 0, 0x7f, 0x03};

static SksePairKey key(uint8_t event) {
    return (SksePairKey){.profile = {1}, .slot = {2}, .event = {event}};
}

static SkseEventTrace trace_for(const SkseGameScope *scope, SkyrimLifecycleRow *rows) {
    SkseEventTrace trace = {.rows = rows};
    memcpy(trace.launch, scope->launch, sizeof trace.launch);
    trace.count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &trace.dropped);
    assert(!trace.dropped);
    return trace;
}

static SksePairGeneration seed_core(const char *root, int source) {
    static const uint8_t companion[] = {
        'S','K','S','E', 1,0,0,0, 0x30,0,7,1, 0,0,0x20,9, 1,0,0,0,
        0,0,0,0, 1,0,0,0, 51,0,0,0, 'S','D','O','M', 0,0,0,0, 39,0,0,0,
        3, 10,0,'S','k','y','r','i','m','.','e','s','m',
        11,0,'F','i','x','t','u','r','e','.','e','s','p',
        11,0,'R','e','m','o','v','e','d','.','e','s','p'
    };
    char location[PATH_MAX];
    int length = snprintf(location, sizeof location, "%s/text-en-voice-en", root);
    assert(length > 0 && (size_t)length < sizeof location);
    SksePairStore *store = NULL;
    assert(skse_pair_open(location, &store) == SKSE_PAIR_OK);
    SksePairStage *stage = NULL;
    SksePairGeneration base = {key(39), source};
    assert(skse_pair_import(&base.key, companion, sizeof companion, &stage) == SKSE_PAIR_OK);
    assert(skse_pair_pin(stage, &base) == SKSE_PAIR_OK);
    assert(skse_pair_commit(store, stage, &base) == SKSE_PAIR_OK);
    skse_pair_discard(stage);
    skse_pair_close(store);
    return base;
}

static void publish(SkseEventBridge *bridge, const SkseGameScope *scope, uint8_t event, int source,
    const SksePairGeneration *base, const SksePluginRecordSet *records, const SksePairRecord *rows, size_t count) {
    SkyrimLifecycleCall slot = skyrim_lifecycle_begin(SkyrimSaveSlot);
    SkseGameEvent request = {*scope, key(event), slot.ticket};
    SksePendingSave *save = NULL;
    SksePairResult detail;
    assert(skse_bridge_begin_save(bridge, &request, base, &save, &detail) == SKSE_BRIDGE_OK);
    SkyrimLifecycleCall vm = skyrim_lifecycle_begin(SkyrimSaveVM);
    if (records) assert(skse_bridge_write_interpreted(save, records) == SKSE_PAIR_OK);
    if (count)
        assert(skse_bridge_write_owner(save, records ? Otherwise : Owner, rows, count) == SKSE_PAIR_OK);
    skyrim_lifecycle_end(vm, SkyrimReturnedTrue);
    skyrim_lifecycle_end(slot, SkyrimNoResult);
    SkyrimLifecycleRow saved[SkyrimLifecycleCapacity];
    SkseEventTrace trace = trace_for(scope, saved);
    SkseBaseEvidence proof = {request, SKSE_BASE_SAVED, source};
    assert(skse_bridge_capture_save(save, &trace, &proof, &detail) == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK);
    if (records) assert(skse_bridge_write_interpreted(save, records) == SKSE_PAIR_SEALED);
    assert(skse_bridge_publish(save, scope, &detail) == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK);
    skse_bridge_drop(save);
}

typedef struct {
    int source, substitute;
    uint8_t event;
    uint32_t uid, vm;
    SkseBridgeResult expected;
    SksePairResult detail;
} Load;

static int restore(SkseEventBridge *bridge, const SkseGameScope *scope, int scratch, Load load,
                   SksePluginRecordSet **out, SkseInterpretedModMap *catalog) {
    SkyrimLifecycleCall slot = skyrim_lifecycle_begin(SkyrimLoadSlot);
    SkseGameEvent request = {*scope, key(load.event), slot.ticket};
    SkseLoadInput *input = NULL;
    int guest = -1;
    SksePairResult detail;
    SkseBridgeResult result = skse_bridge_prepare_load_input(bridge, &request, load.source, scratch,
                                                           &input, &guest, &detail);
    if (result == SKSE_BRIDGE_PAIR && detail == SKSE_PAIR_UNSUPPORTED) {
        skyrim_lifecycle_end(slot, SkyrimReturnedFalse);
        return 0;
    }
    assert(result == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK && guest >= 0);
    unsigned char marker;
    assert(read(guest, &marker, 1) == 1 && (marker == 'a' || marker == 'b'));
    SkyrimLifecycleCall revert = skyrim_lifecycle_begin(SkyrimRevertForLoad);
    skyrim_lifecycle_end(revert, SkyrimNoResult);
    SkyrimLifecycleCall vm = skyrim_lifecycle_begin(SkyrimLoadVM);
    skyrim_lifecycle_end(vm, load.vm);
    skyrim_lifecycle_end(slot, SkyrimReturnedTrue);
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    SkseEventTrace trace = trace_for(scope, rows);
    SkseBaseEvidence loading = {request, SKSE_BASE_LOADED, load.substitute < 0 ? guest : load.substitute};
    assert(skse_bridge_finish_interpreted_load(&input, &trace, &loading, 0, out, catalog, &detail) == SKSE_BRIDGE_ARGUMENT);
    assert(input && !*out);
    assert(skse_bridge_finish_interpreted_load(&input, &trace, &loading, load.uid, out, catalog, &detail) == load.expected);
    assert(detail == load.detail && !input && !!*out == (load.expected == SKSE_BRIDGE_OK));
    assert(lseek(guest, 0, SEEK_CUR) == 1);
    assert(close(guest) == 0);
    return 1;
}

static int base_input(int scratch, const char *name, const char *content) {
    size_t size = strlen(content);
#if defined(__linux__)
    int write_fd = memfd_create(name, MFD_ALLOW_SEALING | MFD_CLOEXEC);
    assert(write_fd >= 0 && write(write_fd, content, size) == (ssize_t)size);
    assert(fcntl(write_fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE) == 0);
    char path[64];
    int length = snprintf(path, sizeof path, "/proc/self/fd/%d", write_fd);
    assert(length > 0 && (size_t)length < sizeof path);
    int reader = open(path, O_RDONLY | O_CLOEXEC);
    (void)scratch;
#else
    int write_fd = openat(scratch, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(write_fd >= 0 && write(write_fd, content, size) == (ssize_t)size);
    int reader = openat(scratch, name, O_RDONLY | O_CLOEXEC);
#endif
    assert(reader >= 0 && close(write_fd) == 0);
    return reader;
}

static SksePluginSession *session(const SkseTrialDependency fixture, uint32_t base, uint32_t handle) {
    SkseTrialOptions options = {0x01070030u, 0x09200000u, 0, 48, handle, 1, 1};
    SkseTrialRuntime runtime = {.serialization = 1, .guest_base = base};
    SksePluginSession *vm = skse_plugin_session_open(fixture, NULL, 0, options, runtime, NULL);
    assert(vm);
    assert(skse_plugin_session_step(vm, SKSE_SESSION_POST_LOAD, WorkLimit, 100) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(vm, SKSE_SESSION_POST_POST_LOAD, WorkLimit, 110) == SKSE_SESSION_OK);
    return vm;
}

static int restore_owned_storage(const char *dll, const char *root, const char *scratch_path, const char *snapshot) {
    uint8_t initializers[8];
    assert(owned_storage_read_initializers(snapshot, initializers));
    int scratch = open(scratch_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(scratch >= 0);
    int original = base_input(scratch, "original.ess", "authored first save bytes");
    int changed = base_input(scratch, "changed.ess", "base bytes for the next save");
    const SkseGameScope scope = {.launch = {7}, .profile = {1}};
    SkseEventBridge *bridge = NULL;
    assert(skse_bridge_open(root, &scope, &bridge, NULL) == SKSE_BRIDGE_OK);
    SkseTrialRecordProbe probe = {0};
    owned_storage_probe(dll, initializers, false, &probe);
    owned_storage_expect(probe.output, false, false);
    SksePluginRecordSet *input = owned_storage_add_integer(probe.output);
    skse_plugin_record_set_close(probe.output);
    probe = (SkseTrialRecordProbe){.input = input, .callback_work_limit = 250000};
    owned_storage_probe(dll, initializers, false, &probe);
    owned_storage_expect(probe.output, false, true);
    uint32_t uid = skse_plugin_record_set_uid(probe.output);
    publish(bridge, &scope, 1, original, NULL, probe.output, NULL, 0);
    skse_plugin_record_set_close(probe.output);
    skse_plugin_record_set_close(input);
    skse_bridge_close(bridge);

    assert(skse_bridge_open(root, &scope, &bridge, NULL) == SKSE_BRIDGE_OK);
    SksePluginRecordSet *saved = NULL;
    const Load wrong = {changed, -1, 1, uid, SkyrimReturnedTrue, SKSE_BRIDGE_PAIR, SKSE_PAIR_WRONG_GENERATION};
    int supported = restore(bridge, &scope, scratch, wrong, &saved, NULL);
    if (supported) {
        assert(!saved);
        const Load correct = {original, -1, 1, uid, SkyrimReturnedTrue, SKSE_BRIDGE_OK, SKSE_PAIR_OK};
        assert(restore(bridge, &scope, scratch, correct, &saved, NULL));
        owned_storage_expect(saved, false, true);
        probe = (SkseTrialRecordProbe){.input = saved, .callback_work_limit = 250000};
        owned_storage_probe(dll, initializers, true, &probe);
        owned_storage_expect(probe.output, true, true);
        skse_plugin_record_set_close(probe.output);
        skse_plugin_record_set_close(saved);
    }
    skse_bridge_close(bridge);
    assert(!close(original) && !close(changed) && !close(scratch));
    if (!supported) return 77;
    puts("Owned StorageUtil restored a signed integer from the exact persisted generation.");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 5) return restore_owned_storage(argv[1], argv[2], argv[3], argv[4]);
    assert(argc == 4);
    uint8_t file[64 * 1024];
    FILE *dll = fopen(argv[1], "rb");
    assert(dll);
    size_t length = fread(file, 1, sizeof file, dll);
    assert(length && feof(dll) && !ferror(dll) && !fclose(dll));
    SkseTrialDependency fixture = {"Fixture.dll", file, length};
    int scratch = open(argv[3], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(scratch >= 0);
    int a = base_input(scratch, "original.ess", "authored first save bytes");
    int b = base_input(scratch, "second.ess", "base bytes for the next save");
    SkseEventBridge *bridge = NULL;
    SkseGameScope scope = {.launch = {3}, .profile = {1}};
    assert(skse_bridge_open(argv[2], &scope, &bridge, NULL) == SKSE_BRIDGE_OK);
    SksePairGeneration initial = seed_core(argv[2], a);
    SksePluginSession *vm = session(fixture, 0, 59);
    SksePluginRecordSet *older = NULL, *newer = NULL, *empty = NULL;
    assert(skse_plugin_session_save(vm, WorkLimit, 120, &older) == SKSE_SESSION_OK);
    assert(skse_plugin_session_save(vm, WorkLimit, 130, &newer) == SKSE_SESSION_OK);
    skse_plugin_session_close(vm, NULL);
    const SksePairRecord foreign = {0x1234, 8, sizeof opaque, opaque};
    publish(bridge, &scope, 1, a, &initial, older, &foreign, 1);
    SksePairGeneration first = {key(1), a}, second = {key(2), b};
    publish(bridge, &scope, 2, b, &first, newer, NULL, 0);
    assert(skse_plugin_record_set_create(Owner, NULL, 0, &empty) == SKSE_SESSION_OK);
    publish(bridge, &scope, 3, b, &second, empty, NULL, 0);
    skse_plugin_record_set_close(empty);
    skse_plugin_record_set_close(older);
    skse_plugin_record_set_close(newer);
    const char big[SKSE_X86_RECORD_BYTES + 1] = {0};
    const SksePairRecord oversized = {1, 1, sizeof big, big};
    publish(bridge, &scope, 4, a, NULL, NULL, &oversized, 1);
    SksePairRecord too_many[SKSE_X86_RECORD_LIMIT + 1] = {{0}};
    publish(bridge, &scope, 5, a, NULL, NULL, too_many, SKSE_X86_RECORD_LIMIT + 1);
    publish(bridge, &scope, 6, a, NULL, NULL, &foreign, 1);
    skse_bridge_close(bridge);

    assert(skse_bridge_open(argv[2], &scope, &bridge, NULL) == SKSE_BRIDGE_OK);
    vm = session(fixture, 0x60000000, 60);
    const Load cases[] = {
        {a, -1, 1, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_OK, SKSE_PAIR_OK},
        {b, -1, 2, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_OK, SKSE_PAIR_OK},
        {a, -1, 1, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_OK, SKSE_PAIR_OK},
        {b, -1, 3, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_OK, SKSE_PAIR_OK},
        {b, -1, 3, Otherwise, SkyrimReturnedTrue, SKSE_BRIDGE_OK, SKSE_PAIR_OK},
        {b, a, 2, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_PAIR, SKSE_PAIR_WRONG_GENERATION},
        {a, -1, 2, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_PAIR, SKSE_PAIR_WRONG_GENERATION},
        {a, -1, 22, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_PAIR, SKSE_PAIR_MISSING},
        {a, -1, 1, Owner, SkyrimReturnedFalse, SKSE_BRIDGE_REJECTED, SKSE_PAIR_OK},
        {a, -1, 4, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_PAIR, SKSE_PAIR_LIMIT},
        {a, -1, 5, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_PAIR, SKSE_PAIR_LIMIT},
        {a, -1, 6, Owner, SkyrimReturnedTrue, SKSE_BRIDGE_OK, SKSE_PAIR_OK}
    };
    const uint32_t expected_counts[] = {2, 3, 2};
    static const SkseLoadedMod active[] = {
        {(const uint8_t *)"Skyrim.esm", 10, 0, SKSE_LOADED_CLASSIC},
        {(const uint8_t *)"fixture.ESP", 11, 4, SKSE_LOADED_CLASSIC}
    };
    SkseInterpretedModMap catalog = {.current = active, .current_count = sizeof active / sizeof active[0]};
    int supported = 1;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        SksePluginRecordSet *restored = NULL;
        supported = restore(bridge, &scope, scratch, cases[i], &restored, &catalog);
        if (!supported) { assert(i == 0); break; }
        if (i < 5) assert(catalog.status == SKSE_MAP_OK && catalog.map.ready);
        else if (cases[i].event == 6) assert(catalog.status == SKSE_MAP_HISTORY_MISSING && !catalog.map.ready);
        else assert(catalog.status == SKSE_MAP_SOURCE_ERROR && !catalog.map.ready);
        if (i < 3) {
            const SkseSerialResolvers resolvers = skse_interpreted_resolvers(&catalog.map);
            assert(skse_plugin_session_revert(vm, WorkLimit, 140) == SKSE_SESSION_OK);
            assert(skse_plugin_session_load_resolved(vm, restored, &resolvers, WorkLimit, 150) == SKSE_SESSION_OK);
            SksePluginRecordSet *check = NULL;
            assert(skse_plugin_session_save(vm, WorkLimit, 160, &check) == SKSE_SESSION_OK);
            assert(skse_plugin_record_set_count(check) == expected_counts[i]);
            skse_plugin_record_set_close(check);
            if (i == 2) {
                assert(skse_plugin_session_load(vm, restored, WorkLimit, 170) == SKSE_SESSION_FAULTED);
                assert(skse_plugin_session_report(vm)->serialization_fault.status == SKSE_X86_UNSUPPORTED_API);
            }
        } else if (i == 3) {
            assert(skse_plugin_record_set_uid(restored) == Owner && !skse_plugin_record_set_count(restored));
        } else if (i == 4) {
            SksePluginRecord record;
            assert(skse_plugin_record_set_count(restored) == 1 && skse_plugin_record_set_at(restored, 0, &record));
            assert(record.type == foreign.type && record.version == foreign.version && record.length == sizeof opaque);
            assert(!memcmp(record.data, opaque, sizeof opaque));
        }
        skse_plugin_record_set_close(restored);
    }
    assert(!supported || skse_plugin_session_report(vm)->serialization_callbacks == 9);
    assert(lseek(a, 0, SEEK_CUR) == 0 && lseek(b, 0, SEEK_CUR) == 0);
    skse_plugin_session_close(vm, NULL);
    skse_bridge_close(bridge);
    assert(!close(a) && !close(b) && !close(scratch));
    if (!supported) return 77;
    puts("Interpreted DLL restored m,n,m from verified generations; rejected inputs dispatched no callbacks.");
    return 0;
}
