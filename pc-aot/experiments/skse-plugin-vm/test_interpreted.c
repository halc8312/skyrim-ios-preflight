/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_trial_cases.h"
#ifdef SKSE_TEST_COSAVE_WIRE
#include "records.h"
#include "x86_skse_records.h"
#endif

static size_t read_owned_dll(const char *path, uint8_t *bytes, size_t capacity) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t length = fread(bytes, 1, capacity, file);
    int unusable = ferror(file) || !feof(file);
    fclose(file);
    return unusable ? 0 : length;
}

static uint32_t read_authored_asset(void *context, const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    (void)context;
    const char *content = NULL;
    if (!strcmp(path, "Data/SKSE/Plugins/Settings.ini")) content = "ABCDEFG";
    if (!strcmp(path, "Data/SKSE/Plugins/\xe8\xa8\xad\xe5\xae\x9a.ini")) content = "jp\r\n";
    if (!content) return 2;
    *length = strlen(content);
    if (*length > limit) return 223;
    *bytes = malloc(*length);
    if (!*bytes) return 8;
    memcpy(*bytes, content, *length);
    return 0;
}

static uint32_t authored_attributes(void *context, const char *path, uint32_t *out) {
    (void)context;
    if (!strcmp(path, "Data/SKSE")) *out = SKSE_WIN_ATTRIBUTE_DIRECTORY;
    else if (!strcmp(path, "Data/SKSE/Plugins/\xe8\xa8\xad\xe5\xae\x9a.ini")) *out = SKSE_WIN_ATTRIBUTE_NORMAL;
    else return 2;
    return 0;
}

static SkseTrialResult resident_messages(const uint8_t *bytes, size_t length, SkseTrialOptions options, SkseTrialRuntime runtime) {
    SkseTrialResult result;
    const SkseTrialDependency primary = {"Fixture.dll", bytes, length};
    SksePluginSession *session = skse_plugin_session_open(primary, NULL, 0, options, runtime, &result);
    if (!session) return result;
    assert(result.messages_delivered == 0);
    if (skse_plugin_session_step(session, SKSE_SESSION_POST_LOAD, 10000, runtime.uptime_ms + 10) == SKSE_SESSION_OK)
        skse_plugin_session_step(session, SKSE_SESSION_POST_POST_LOAD, 10000, runtime.uptime_ms + 20);
    skse_plugin_session_close(session, &result);
    return result;
}

static const uint32_t serial_budget = 10000;

#ifdef SKSE_TEST_COSAVE_WIRE
typedef struct {
    SksePluginRecord entries[SKSE_X86_RECORD_LIMIT];
    uint32_t count;
} CompanionRecords;

static int collect_record(void *context, const SksePairRecord *row) {
    CompanionRecords *records = context;
    if (records->count == SKSE_X86_RECORD_LIMIT) return 0;
    records->entries[records->count++] = (SksePluginRecord){row->type, row->version, row->length, row->data};
    return 1;
}

static SksePluginRecordSet *companion_roundtrip(const SksePluginRecordSet *saved) {
    const uint32_t count = skse_plugin_record_set_count(saved);
    const uint32_t uid = skse_plugin_record_set_uid(saved);
    SksePairRecord entries[SKSE_X86_RECORD_LIMIT];
    SksePluginRecord row = {.type = UINT32_MAX};
    assert(count <= SKSE_X86_RECORD_LIMIT);
    assert(!skse_plugin_record_set_at(saved, count, &row) && row.type == UINT32_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        assert(skse_plugin_record_set_at(saved, i, &row));
        entries[i] = (SksePairRecord){row.type, row.version, row.length, row.data};
    }
    SksePairImage image = {0};
    assert(skse_records_empty(&image) == SKSE_PAIR_OK);
    assert(skse_records_replace(&image, uid, entries, count) == SKSE_PAIR_OK);
    FILE *disk = tmpfile();
    assert(disk);
    assert(fwrite(image.data, 1, image.size, disk) == image.size);
    assert(!fflush(disk) && !fseek(disk, 0, SEEK_SET));
    free(image.data);
    image.data = malloc(image.size);
    assert(image.data && fread(image.data, 1, image.size, disk) == image.size);
    assert(fgetc(disk) == EOF && !ferror(disk));
    assert(!fclose(disk));
    assert(skse_records_validate(image.data, image.size) == SKSE_PAIR_OK);
    CompanionRecords restored = {0};
    assert(skse_records_visit(&image, uid, collect_record, &restored) == SKSE_PAIR_OK);
    assert(restored.count == count);
    SksePluginRecordSet *copy = NULL;
    assert(skse_plugin_record_set_create(0, restored.entries, restored.count, &copy) == SKSE_SESSION_ARGUMENT && !copy);
    const SksePluginRecord invalid = {1, 1, SKSE_X86_RECORD_BYTES + 1, "bad"};
    assert(skse_plugin_record_set_create(uid, &invalid, 1, &copy) == SKSE_SESSION_ARGUMENT && !copy);
    assert(skse_plugin_record_set_create(uid, restored.entries, restored.count, &copy) == SKSE_SESSION_OK);
    free(image.data);
    for (uint32_t i = 0; i < count; ++i) {
        assert(skse_plugin_record_set_at(copy, i, &row));
        assert(row.type == entries[i].type && row.version == entries[i].version && row.length == entries[i].length);
        if (row.length) assert(memcmp(row.data, entries[i].data, row.length) == 0);
        else assert(!row.data);
    }
    return copy;
}
#endif

static void start_serial_session(SksePluginSession *session) {
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_LOAD, serial_budget, 100) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_POST_LOAD, serial_budget, 110) == SKSE_SESSION_OK);
}

static void callback_export_failures(const uint8_t *bytes, size_t length) {
    SkseTrialOptions options = trial_cases[0];
    SkseTrialRuntime runtime = {.serialization = 1, .static_tls = 1};
    const uint32_t site = 0x00600000;
    const SkseTrialDependency plugin = {"Fixture.dll", bytes, length};

    // Capture matching data first so the observed-hook cases replay a real input.
    options.plugin_handle = 67;
    runtime.hook_probe_sites = &site;
    runtime.hook_probe_count = 1;
    SkseTrialRecordProbe clean = {0};
    SkseTrialResult result = skse_plugin_trial_with_record_probe(bytes, length, options, runtime, &clean);
    assert(result.status == SKSE_TRIAL_ENGINE_HOOKS_OBSERVED && result.load.hook_count == 1 &&
           result.load.hooks[0].site == site && result.detach.status == SKSE_X86_OK);
    assert(clean.status == SKSE_SESSION_OK && clean.output);

    for (unsigned owner = 65; owner <= 67; ++owner) {
        options.plugin_handle = owner;
        SkseTrialRecordProbe probe = {.input = clean.output};
        result = skse_plugin_trial_with_record_probe(bytes, length, options, runtime, &probe);
        assert(result.status == SKSE_TRIAL_ENGINE_HOOKS_OBSERVED && result.load.hook_count == 1);
        assert(result.serialization_callbacks == 3);
        if (owner == 67) {
            assert(probe.status == SKSE_SESSION_OK && probe.output);
            skse_plugin_record_set_close(probe.output);
        } else {
            assert(owner == 65 ? result.detach.status != SKSE_X86_OK : result.tls_fault.status != SKSE_X86_OK);
            assert(probe.status == SKSE_SESSION_FAULTED && !probe.output);
        }
    }
    skse_plugin_record_set_close(clean.output);
    puts("record export discarded after both observed detach failures");

    runtime.hook_probe_count = 0;
    for (unsigned owner = 63; owner <= 64; ++owner) {
        options.plugin_handle = owner;
        SksePluginSession *session = skse_plugin_session_open(plugin, NULL, 0, options, runtime, NULL);
        assert(session);
        start_serial_session(session);
        SksePluginRecordSet *output = NULL;
        assert(skse_plugin_session_save(session, serial_budget, 120, &output) == SKSE_SESSION_FAULTED && !output);
        assert(skse_plugin_session_report(session)->status == SKSE_TRIAL_SERIALIZATION_FAULT);
        assert(skse_plugin_session_report(session)->serialization_fault.status == SKSE_X86_RECORD_WRITE_FAILED);
        skse_plugin_session_close(session, NULL);

        SkseTrialRecordProbe probe = {0};
        result = skse_plugin_trial_with_record_probe(bytes, length, options, runtime, &probe);
        assert(result.status == SKSE_TRIAL_SERIALIZATION_FAULT);
        assert(result.serialization_fault.status == SKSE_X86_RECORD_WRITE_FAILED);
        assert(probe.status == SKSE_SESSION_FAULTED && !probe.output);
    }
    puts("record export discarded after both ignored write limits");
}

static void resident_serialization(const uint8_t *bytes, size_t length) {
    const SkseTrialDependency plugin = {"Fixture.dll", bytes, length};
    SkseTrialRuntime runtime = {.serialization = 1};
    SkseTrialOptions options = trial_cases[0];
    SksePluginSession *owner = skse_plugin_session_open(plugin, NULL, 0, options, runtime, NULL);
    assert(owner);
    SksePluginRecordSet *first = NULL, *second = NULL, *retaken = NULL;
    assert(skse_plugin_session_save(owner, serial_budget, 0, &first) == SKSE_SESSION_OUT_OF_ORDER);
    assert(!first);
    start_serial_session(owner);
    assert(skse_plugin_session_save(owner, 0, 120, &first) == SKSE_SESSION_ARGUMENT);
    assert(skse_plugin_session_save(owner, serial_budget, 120, &first) == SKSE_SESSION_OK);
    assert(skse_plugin_record_set_uid(first) == 0x1ced1591 && skse_plugin_record_set_count(first) == 2);
    assert(skse_plugin_session_save(owner, serial_budget, 120, &first) == SKSE_SESSION_ARGUMENT);
    assert(skse_plugin_session_save(owner, serial_budget, 130, &second) == SKSE_SESSION_OK);
    assert(skse_plugin_record_set_count(first) == 2 && skse_plugin_record_set_count(second) == 3);

    options.plugin_handle = 61;
    SksePluginSession *different = skse_plugin_session_open(plugin, NULL, 0, options, runtime, NULL);
    assert(different);
    start_serial_session(different);
    assert(skse_plugin_session_load(different, first, serial_budget, 120) == SKSE_SESSION_UID_MISMATCH);
    assert(!skse_plugin_session_report(different)->serialization_callbacks);
    assert(skse_plugin_session_save(different, serial_budget, 130, &retaken) == SKSE_SESSION_OK);
    assert(skse_plugin_record_set_uid(retaken) == 0x1ced1592 && skse_plugin_record_set_count(retaken) == 2);
    skse_plugin_record_set_close(retaken);
    retaken = NULL;
    skse_plugin_session_close(different, NULL);
    assert(skse_plugin_session_report(owner)->serialization_callbacks == 2);
    assert(skse_plugin_record_set_count(first) == 2 && skse_plugin_record_set_count(second) == 3);
    skse_plugin_session_close(owner, NULL);

#ifdef SKSE_TEST_COSAVE_WIRE
    SksePluginRecordSet *first_companion = companion_roundtrip(first);
    SksePluginRecordSet *second_companion = companion_roundtrip(second);
    skse_plugin_record_set_close(first);
    skse_plugin_record_set_close(second);
    first = first_companion;
    second = second_companion;
    puts("Classic companion records verified before restoring the fresh session");
#endif

    options.plugin_handle = 60; // The plugin handle can change across launches.
    runtime.guest_base = 0x60000000;
    SksePluginSession *reopened = skse_plugin_session_open(plugin, NULL, 0, options, runtime, NULL);
    assert(reopened);
    assert(skse_plugin_session_load(reopened, first, serial_budget, 0) == SKSE_SESSION_OUT_OF_ORDER);
    start_serial_session(reopened);
    const SksePluginRecordSet *inputs[] = {first, second, first};
    const uint32_t expected_counts[] = {2, 3, 2};
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        /* Revert clears the fixture's current value. Load reads the payload
         * and restores it. The next save observes that value (m or n). */
        assert(skse_plugin_session_revert(reopened, serial_budget, 120) == SKSE_SESSION_OK);
        assert(skse_plugin_session_load(reopened, inputs[i], serial_budget, 130) == SKSE_SESSION_OK);
        assert(skse_plugin_session_save(reopened, serial_budget, 140, &retaken) == SKSE_SESSION_OK);
        assert(skse_plugin_record_set_count(retaken) == expected_counts[i]);
        skse_plugin_record_set_close(retaken);
        retaken = NULL;
    }
    assert(skse_plugin_session_report(reopened)->serialization_callbacks == 9);
    assert(skse_plugin_session_load(reopened, first, 1, 150) == SKSE_SESSION_FAULTED);
    assert(skse_plugin_session_report(reopened)->serialization_fault.status == SKSE_X86_INSTRUCTION_LIMIT);
    assert(skse_plugin_session_revert(reopened, serial_budget, 160) == SKSE_SESSION_FAULTED);
    skse_plugin_session_close(reopened, NULL);

    options.plugin_handle = 62;
    SksePluginSession *changing = skse_plugin_session_open(plugin, NULL, 0, options, runtime, NULL);
    assert(changing);
    start_serial_session(changing);
    assert(skse_plugin_session_save(changing, serial_budget, 120, &retaken) == SKSE_SESSION_FAULTED);
    assert(!retaken && !skse_plugin_session_report(changing)->serialization_callbacks);
    assert(skse_plugin_session_report(changing)->serialization_fault.status == SKSE_X86_UNSUPPORTED_API);
    skse_plugin_session_close(changing, NULL);
    skse_plugin_record_set_close(first);
    skse_plugin_record_set_close(second);
    puts("resident serialization restored m,n,m in a fresh session; wrong and changed UIDs refused");
}

static SkseTrialResult resident_papyrus(const uint8_t *bytes, size_t length, SkseTrialOptions options,
                                        SkseTrialRuntime runtime, unsigned *attempted, unsigned *accepted) {
    SkseTrialResult result;
    const SkseTrialDependency primary = {"Fixture.dll", bytes, length};
    runtime.papyrus_probe_epoch = 41;
    SksePluginSession *session = skse_plugin_session_open(primary, NULL, 0, options, runtime, &result);
    if (!session) return result;
    SkseTrialResult standalone = skse_plugin_trial_with_runtime(bytes, length, options, runtime);
    assert(standalone.status == SKSE_TRIAL_LOAD_DECLINED || standalone.status == SKSE_TRIAL_LOAD_FAULT);
    SkseX86Result callback = {.eax = 42};
    SksePapyrusTrialReport report = skse_plugin_session_probe_papyrus(session, 41, 10000, 1, &callback);
    assert(report.status == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER && callback.eax == 42);
    start_serial_session(session);
    report = skse_plugin_session_probe_papyrus(session, 40, 10000, 1, &callback);
    assert(report.status == SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED && callback.eax == 42);
    report = skse_plugin_session_probe_papyrus(session, 41, 10000, 1, &callback);
    assert(report.status == SKSE_PAPYRUS_TRIAL_OK && report.attempted == 1);
    assert(callback.status == SKSE_X86_OK && callback.instructions && (uint8_t)callback.eax == 1);
    *attempted = (unsigned)report.attempted;
    *accepted = (uint8_t)callback.eax;
    report = skse_plugin_session_probe_papyrus(session, 41, 10000, 1, &callback);
    assert(report.status == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    skse_plugin_session_close(session, &result);

    SksePluginSession *limited = skse_plugin_session_open(primary, NULL, 0, options, runtime, NULL);
    assert(limited);
    start_serial_session(limited);
    report = skse_plugin_session_probe_papyrus(limited, 41, 1, 1, &callback);
    assert(report.status == SKSE_PAPYRUS_TRIAL_CALLBACK_FAULT && report.attempted == 1);
    assert(callback.status == SKSE_X86_INSTRUCTION_LIMIT);
    assert(skse_plugin_session_report(limited)->status == SKSE_TRIAL_PAPYRUS_FAULT);
    skse_plugin_session_close(limited, NULL);
    return result;
}

static void resident_papyrus_native(const uint8_t *bytes, size_t length, bool moved, bool bad_type) {
    const SkseTrialDependency primary = {"Fixture.dll", bytes, length};
    const SkseTrialRuntime runtime = {
        .guest_base = moved ? UINT32_C(0x60000000) : 0,
        .papyrus_probe_epoch = 41, .papyrus_fixture_int0 = true
    };
    const uint32_t sentinel = UINT32_C(0x55aa33cc);
    SkseTrialResult report;
    SksePluginSession *session = skse_plugin_session_open(primary, NULL, 0, trial_cases[0], runtime, &report);
    if (!session)
        fprintf(stderr, "Papyrus fixture open=%u, PE=%u Load=%u at 0x%08x\n",
                (unsigned)report.status, (unsigned)report.image_status,
                (unsigned)report.load.status, report.load.instruction_va);
    assert(session);
    uint32_t bits = sentinel;
    SkseX86Result call = {.eax = 42};
    assert(skse_plugin_session_run_papyrus_fixture(session, 1000, 10, &bits, &call) == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    assert(bits == sentinel && call.eax == 42);
    start_serial_session(session);
    SksePapyrusTrialReport registration = skse_plugin_session_probe_papyrus(session, 41, 1000, 10, &call);
    if (registration.status != SKSE_PAPYRUS_TRIAL_OK)
        fprintf(stderr, "Papyrus fixture registration=%u, x86=%u at 0x%08x\n",
                (unsigned)registration.status, (unsigned)call.status, call.instruction_va);
    assert(registration.status == SKSE_PAPYRUS_TRIAL_OK && registration.attempted == 1);
    assert(call.status == SKSE_X86_OK && (uint8_t)call.eax == 1);

    assert(skse_plugin_session_run_papyrus_fixture(session, 0, 10, &bits, NULL) == SKSE_PAPYRUS_TRIAL_ARGUMENT);
    assert(bits == sentinel);
    SksePapyrusTrialStatus ran = skse_plugin_session_run_papyrus_fixture(session, 1000, 12, &bits, &call);
    if (call.status != SKSE_X86_OK)
        fprintf(stderr, "Papyrus fixture Run=%u, x86=%u at 0x%08x\n",
                (unsigned)ran, (unsigned)call.status, call.instruction_va);
    assert(call.status == SKSE_X86_OK && (uint8_t)call.eax == 1);
    if (bad_type) {
        assert(ran == SKSE_PAPYRUS_TRIAL_RESULT_REFUSED && bits == sentinel);
        assert(skse_plugin_session_report(session)->status == SKSE_TRIAL_PAPYRUS_FAULT);
        assert(skse_plugin_session_run_papyrus_fixture(session, 1000, 13, &bits, NULL) ==
               SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    } else {
        assert(ran == SKSE_PAPYRUS_TRIAL_OK && bits == UINT32_C(0xfe123456));
        uint32_t limited = sentinel;
        assert(skse_plugin_session_run_papyrus_fixture(session, 1, 13, &limited, &call) ==
               SKSE_PAPYRUS_TRIAL_NATIVE_FAULT);
        assert(call.status == SKSE_X86_INSTRUCTION_LIMIT && limited == sentinel);
    }
    printf("papyrus-int,%u,%08x\n", ran == SKSE_PAPYRUS_TRIAL_OK, bits);
    skse_plugin_session_close(session, NULL);
}

int main(int argc, char *argv[]) {
    int clock = argc >= 3 && strcmp(argv[2], "clock") == 0;
    int memory = argc >= 3 && strcmp(argv[2], "memory") == 0;
    int allocation = argc >= 3 && strcmp(argv[2], "allocation") == 0;
    int interlocked = argc >= 3 && (!strcmp(argv[2], "interlocked") || !strcmp(argv[2], "locked-instructions"));
    int module_lookup = argc >= 3 && strcmp(argv[2], "module-lookup") == 0;
    int protection = argc >= 3 && strcmp(argv[2], "protection") == 0;
    int exception_frames = argc >= 3 && strcmp(argv[2], "exception-frames") == 0;
    int text_conversion = argc >= 3 && strcmp(argv[2], "text-conversion") == 0;
    int environment = argc >= 3 && strcmp(argv[2], "environment") == 0;
    int file_assets = argc >= 3 && strcmp(argv[2], "file-assets") == 0;
    int events = argc >= 3 && strcmp(argv[2], "events") == 0;
    int worker = argc >= 3 && strcmp(argv[2], "worker") == 0;
    int tls = argc >= 3 && strcmp(argv[2], "tls") == 0;
    int tls_pair = argc >= 4 && strcmp(argv[2], "static-tls-pair") == 0;
    int static_tls = tls_pair || (argc >= 3 && strcmp(argv[2], "static-tls") == 0);
    int resident = argc >= 3 && strcmp(argv[2], "resident-messages") == 0;
    int messages = resident || (argc >= 3 && strcmp(argv[2], "messages") == 0);
    int serialization = argc >= 3 && strcmp(argv[2], "serialization") == 0;
    int papyrus = argc >= 3 && strcmp(argv[2], "papyrus-registration") == 0;
    bool papyrus_bad_type = argc >= 3 && strcmp(argv[2], "papyrus-int-bad") == 0;
    bool papyrus_native = papyrus_bad_type || (argc >= 3 && strcmp(argv[2], "papyrus-int") == 0);
    int serial_resident = argc == 3 && strcmp(argv[2], "resident-serialization") == 0;
    bool serial_export = argc == 3 && strcmp(argv[2], "callback-export-failures") == 0;
    int dependency = tls_pair || (argc >= 4 && strcmp(argv[2], "dependency") == 0);
    int moved = argc >= 4 && strcmp(argv[argc - 1], "move") == 0;
    assert(argc == 2 || dependency ||
        ((clock || memory || allocation || interlocked || module_lookup || file_assets || events || tls || static_tls ||
          messages || serialization || papyrus || worker || protection || exception_frames || text_conversion ||
          environment || serial_resident || serial_export || papyrus_native) &&
         (argc == 3 || moved)));
    uint8_t bytes[64 * 1024];
    size_t length = read_owned_dll(argv[1], bytes, sizeof bytes);
    if (!length) return 2;
    if (papyrus_native) {
        resident_papyrus_native(bytes, length, moved, papyrus_bad_type);
        return 0;
    }
    if (serial_export) {
        callback_export_failures(bytes, length);
        return 0;
    }
    if (serial_resident) {
        resident_serialization(bytes, length);
        return 0;
    }
    uint8_t helper[64 * 1024];
    size_t helper_length = dependency ? read_owned_dll(argv[3], helper, sizeof helper) : 0;
    if (dependency && !helper_length) return 2;
    for (size_t i = 0; i < sizeof trial_cases / sizeof trial_cases[0]; ++i) {
        SkseTrialRuntime runtime = {
            .uptime_ms = UINT32_C(0x9100abcd),
            .guest_base = moved ? UINT32_C(0x60000000) : 0,
            .core_messages = (unsigned)messages, .static_tls = (unsigned)(static_tls || worker),
            .serialization = (unsigned)serialization, .worker_thread = (unsigned)worker,
            .assets = {.read = file_assets ? read_authored_asset : NULL,
                       .attributes = file_assets ? authored_attributes : NULL},
            .module_file_name = module_lookup ? "fixture.dll" : NULL
        };
        SkseTrialResult result;
        unsigned papyrus_attempted = 0, papyrus_accepted = 0;
        if (resident) result = resident_messages(bytes, length, trial_cases[i], runtime);
        else if (papyrus) result = resident_papyrus(bytes, length, trial_cases[i], runtime, &papyrus_attempted, &papyrus_accepted);
        else if (argc >= 3) result = skse_plugin_trial_with_runtime(bytes, length, trial_cases[i], runtime);
        else result = skse_plugin_trial(bytes, length, trial_cases[i]);
        if (dependency) {
            assert(result.status == SKSE_TRIAL_IMAGE_REFUSED && result.image_status == SKSE_PE_UNAPPROVED_IMPORT);
            SkseTrialDependency supplied = {"oWNEDwORKER.DlL", helper, helper_length};
            result = skse_plugin_trial_with_dependency(bytes, length, trial_cases[i], runtime, supplied);
            assert(result.dependency_status == SKSE_PE_OK);
            assert(result.dependency_attach.status == SKSE_X86_OK && result.dependency_attach.instructions);
            assert(result.dependency_detach.status == SKSE_X86_OK && result.dependency_detach.instructions);
            supplied.file_name = "Unrelated.dll";
            SkseTrialResult unrelated = skse_plugin_trial_with_dependency(bytes, length, trial_cases[i], runtime, supplied);
            assert(unrelated.status == SKSE_TRIAL_DEPENDENCY_REFUSED &&
                   unrelated.dependency_status == SKSE_PE_MISSING_DEPENDENCY_EXPORT);
        }
        if (result.status != SKSE_TRIAL_COMPLETED && result.status != SKSE_TRIAL_QUERY_DECLINED &&
            result.status != SKSE_TRIAL_LOAD_DECLINED && result.status != SKSE_TRIAL_MESSAGE_FAULT) {
            fprintf(stderr, "case=%zu phase=%u PE=%u x86-query=%u at=0x%08x x86-load=%u at=0x%08x TLS=%u at=0x%08x serial=%u at=0x%08x worker=%u at=0x%08x\n",
                    i, (unsigned)result.status, (unsigned)result.image_status,
                    (unsigned)result.query.status, result.query.instruction_va,
                    (unsigned)result.load.status, result.load.instruction_va,
                    (unsigned)result.tls_fault.status, result.tls_fault.instruction_va,
                    (unsigned)result.serialization_fault.status, result.serialization_fault.instruction_va,
                    (unsigned)result.worker_fault.status, result.worker_fault.instruction_va);
            return 1;
        }
        unsigned query = result.status != SKSE_TRIAL_QUERY_DECLINED;
        assert(result.query.instructions && result.query.instructions < 1000);
        if (query) assert(result.load.instructions && result.load.instructions < (environment ? 4096u : 1000u));
        if (clock) {
            assert(result.attach.status == SKSE_X86_OK && result.attach.service_calls[2] == 1);
            assert(result.detach.status == SKSE_X86_OK && result.detach.instructions > 0);
            if (result.status == SKSE_TRIAL_COMPLETED) assert(result.load.service_calls[2] == 1);
        }
        if (worker) {
            assert(result.workers_completed == query && result.tls_callbacks == (query ? 4u : 2u));
            assert(result.worker_fault.status == SKSE_X86_OK);
        }
        if (memory) {
            for (unsigned service = 3; service <= 5; ++service) assert(result.query.service_calls[service] == 1);
        }
        if (static_tls) {
            assert(result.tls_callbacks == (tls_pair ? 6u : 4u) && result.tls_fault.status == SKSE_X86_OK);
            assert(result.attach.status == SKSE_X86_OK && result.attach.eax == 1 && result.attach.instructions > 0);
            assert(result.detach.status == SKSE_X86_OK && result.detach.instructions > 0);
            runtime.static_tls = 0;
            SkseTrialResult disabled = skse_plugin_trial_with_runtime(bytes, length, trial_cases[i], runtime);
            assert(disabled.status == SKSE_TRIAL_IMAGE_REFUSED && disabled.image_status == SKSE_PE_UNSUPPORTED_TLS);
        }
        print_trial(i, query, result.status == SKSE_TRIAL_COMPLETED || result.status == SKSE_TRIAL_MESSAGE_FAULT,
                    result.plugin_version, result.name,
                    result.load.service_calls[0], result.load.service_calls[1]);
        if (messages) {
            unsigned fault = result.status == SKSE_TRIAL_MESSAGE_FAULT;
            if (fault) assert(result.message.status == SKSE_X86_UNSUPPORTED_API);
            printf("messages,%u,%u,%u\n", result.messages_delivered, fault, result.unavailable_interface);
        }
        if (serialization) printf("serialization,%u,%u\n", result.serialization_callbacks, result.serialization_records);
        if (papyrus) printf("papyrus,%u,%u\n", papyrus_attempted, papyrus_accepted);
    }
    return 0;
}
