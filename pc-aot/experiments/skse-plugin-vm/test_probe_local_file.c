/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_local_file.h"
#include "fixtures/owned_storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_fixture(const char *path, size_t *length) {
    const size_t capacity = 64 * 1024;
    uint8_t *bytes = malloc(capacity);
    FILE *fixture = fopen(path, "rb");
    assert(bytes && fixture);
    *length = fread(bytes, 1, capacity, fixture);
    assert(*length && feof(fixture) && fclose(fixture) == 0);
    return bytes;
}

static void check_session(const char *path) {
    size_t length;
    uint8_t *bytes = read_fixture(path, &length);
    const SkseTrialDependency plugin = {"Tasks.dll", bytes, length};
    const SkseTrialOptions options = {0x01070030, 0x09200000, 0, 48, 1, 1, 1};
    const SkseTrialRuntime runtime = {.task_interface = 1};
    SkseTrialResult result;
    SksePluginSession *session = skse_plugin_session_open(plugin, NULL, 0, options, runtime, &result);
    SksePluginSession *limited = skse_plugin_session_open(plugin, NULL, 0, options, runtime, NULL);
    free(bytes);
    assert(session && limited && result.status == SKSE_TRIAL_COMPLETED);
    assert(result.tasks_completed[0] == 0 && result.tasks_completed[1] == 0);

    const uint32_t budget = 10000;
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_POST_LOAD, budget, 100) == SKSE_SESSION_OUT_OF_ORDER);
    assert(skse_plugin_session_step(session, SKSE_SESSION_GAME_TASKS, budget, 100) == SKSE_SESSION_OUT_OF_ORDER);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_LOAD, 0, 100) == SKSE_SESSION_ARGUMENT);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_LOAD, UINT32_MAX, 100) == SKSE_SESSION_ARGUMENT);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_LOAD, budget, 100) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_LOAD, budget, 100) == SKSE_SESSION_OUT_OF_ORDER);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_POST_LOAD, budget, 110) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(session, SKSE_SESSION_GAME_TASKS, budget, 120) == SKSE_SESSION_OK);
    const SkseTrialResult *current = skse_plugin_session_report(session);
    assert(current->tasks_completed[0] == 3 && current->tasks_completed[1] == 0);
    assert(skse_plugin_session_report(limited)->tasks_completed[0] == 0);
    assert(skse_plugin_session_step(session, SKSE_SESSION_UI_TASKS, budget, 130) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(session, SKSE_SESSION_UI_TASKS, budget, 140) == SKSE_SESSION_OK);
    skse_plugin_session_close(session, &result);
    assert(result.status == SKSE_TRIAL_COMPLETED && result.tasks_completed[0] == 3 && result.tasks_completed[1] == 1);

    assert(skse_plugin_session_step(limited, SKSE_SESSION_POST_LOAD, budget, 100) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(limited, SKSE_SESSION_POST_POST_LOAD, budget, 110) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(limited, SKSE_SESSION_GAME_TASKS, 1, 120) == SKSE_SESSION_FAULTED);
    assert(skse_plugin_session_step(limited, SKSE_SESSION_UI_TASKS, budget, 130) == SKSE_SESSION_FAULTED);
    skse_plugin_session_close(limited, &result);
    assert(result.status == SKSE_TRIAL_TASK_FAULT && result.task_fault.status == SKSE_X86_INSTRUCTION_LIMIT);
    assert(result.tasks_completed[0] == 0 && result.tasks_completed[1] == 0);
    puts("resident tasks=3/1 independent-lifetime=ok budget-refusal=ok");
}

static void check_worker_session(const char *path) {
    size_t length;
    uint8_t *bytes = read_fixture(path, &length);
    const SkseTrialDependency plugin = {"Worker.dll", bytes, length};
    const SkseTrialOptions options = {0x01070030, 0x09200000, 0, 48, 1, 1, 1};
    const SkseTrialRuntime runtime = {.worker_thread = 1};
    SkseTrialResult result;
    SksePluginSession *session = skse_plugin_session_open(plugin, NULL, 0, options, runtime, &result);
    free(bytes);
    assert(session && !result.workers_completed);
    const uint32_t budget = 10000;
    assert(skse_plugin_session_step(session, SKSE_SESSION_WORKER, budget, 100) == SKSE_SESSION_OUT_OF_ORDER);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_LOAD, budget, 100) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_POST_LOAD, budget, 110) == SKSE_SESSION_OK);
    assert(!skse_plugin_session_report(session)->workers_completed);
    assert(skse_plugin_session_step(session, SKSE_SESSION_WORKER, budget, 120) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(session, SKSE_SESSION_WORKER, budget, 130) == SKSE_SESSION_OK);
    skse_plugin_session_close(session, &result);
    assert(result.status == SKSE_TRIAL_COMPLETED && result.workers_completed == 1);
    puts("resident worker completed once on the explicit step");
}

static int trace_owned_papyrus(const char *dll, const char *snapshot, bool observe_hooks) {
    uint8_t owned_initializers[8];
    if (!owned_storage_read_initializers(snapshot, owned_initializers)) return 2;
    const SkseX86ReadView views[] = {
        {0x0124166c, owned_initializers, 4},
        {0x012b5b80, owned_initializers + 4, 4},
    };
    const uint32_t bases[] = {0, 0x12000000};
    const uint32_t sites[] = {0x006a9e90, 0x008cab54, 0x006f02ff};
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; ++i) {
        const SkseLocalProbeSetup setup = {
            .guest_base = bases[i], .read_views = views, .read_view_count = sizeof views / sizeof views[0],
            .capture_papyrus_startup = true, .hook_probe_sites = sites,
            .hook_probe_count = observe_hooks ? sizeof sites / sizeof sites[0] : 0
        };
        SkseLocalDllReport report;
        SkseProbeFileStatus status = skse_probe_local_dll_with_setup(dll, 4427, &setup, &report);
        const SkseTrialResult *trial = &report.trial;
        printf("snapshot=%u file=%u mapped=%u partial=%u status=%u registered=%u callback=%08x\n", (unsigned)i,
               (unsigned)status, (unsigned)trial->image_status, (unsigned)trial->partial_windows_imports,
               (unsigned)trial->status, trial->papyrus_startup_registrations, trial->first_papyrus_callback);
        const SkseX86Result *phases[] = {&trial->attach, &trial->query, &trial->load};
        const char *labels[] = {"attach", "query", "load"};
        for (size_t j = 0; j < sizeof phases / sizeof phases[0]; ++j) {
            const SkseX86Result *phase = phases[j];
            const char *symbol = skse_trial_windows_service_name(phase->failed_service);
            printf("%s status=%u instructions=%u ip=%08x memory=%08x access=%u service=%s\n",
                   labels[j], (unsigned)phase->status, phase->instructions, phase->instruction_va,
                   phase->fault_address, phase->fault_access, symbol ? symbol : "-");
            if (phase->protection_length)
                printf("protect address=%08x bytes=%u flags=%08x\n", phase->protection_address,
                       phase->protection_length, phase->protection_flags);
        }
        for (unsigned hook = 0; hook < trial->load.hook_count; ++hook) {
            const SkseX86HookObservation *observation = &trial->load.hooks[hook];
            printf("observed hook site=%08x target=%08x\n", observation->site, observation->target);
        }
        if (observe_hooks && (trial->load.hook_count != sizeof sites / sizeof sites[0] ||
                              trial->status == SKSE_TRIAL_COMPLETED)) return 4;
        if (status != SKSE_PROBE_FILE_CHECKED || trial->image_status != SKSE_PE_OK ||
            trial->attach.status != SKSE_X86_OK || !trial->attach.instructions ||
            trial->query.status != SKSE_X86_OK || (uint8_t)trial->query.eax != 1 || !trial->load.instructions)
            return 3;
    }
    return 0;
}

static int audit_owned_papyrus(const char *dll, const char *snapshot, const char *code_path) {
    uint8_t words[8], *code;
    size_t length;
    if (!owned_storage_read_initializers(snapshot, words) ||
        skse_probe_snapshot_file(code_path, &code, &length) != SKSE_PROBE_FILE_CHECKED) return 2;
    SksePeFileRegion text;
    SksePeStatus admitted = skse_pe_executable_code(code, length, &text);
    if (admitted != SKSE_PE_OK) {
        printf("code view eligibility=%u\n", (unsigned)admitted);
        free(code);
        return 2;
    }
    const SkseX86ReadView views[] = {
        {0x0124166c, words, 4}, {0x012b5b80, words + 4, 4}, {text.address, code + text.file_offset, text.length}
    };
    SkseX86CodeAudit audit = {.read_view_index = 2};
    const SkseLocalProbeSetup setup = {
        .read_views = views, .read_view_count = sizeof views / sizeof views[0],
        .capture_papyrus_startup = true, .code_audit = &audit
    };
    SkseLocalDllReport report;
    SkseProbeFileStatus file_status = skse_probe_local_dll_with_setup(dll, 4427, &setup, &report);
    printf("audit status=%u load=%u writes=%zu truncated=%u\n", (unsigned)report.trial.status,
           (unsigned)report.trial.load.status, audit.count, (unsigned)audit.truncated);
    free(code);
    if (file_status != SKSE_PROBE_FILE_CHECKED || report.trial.status != SKSE_TRIAL_ENGINE_HOOKS_OBSERVED ||
        audit.truncated || audit.count != 6) return 3;
    const uint32_t sites[] = {0x006a9e90, 0x008cab54, 0x006f02ff};
    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; ++i) {
        const SkseX86CodeWrite *opcode = &audit.writes[i*2], *displacement = opcode + 1;
        if (opcode->address != sites[i] || opcode->width != 1 || opcode->value != 0xe9 ||
            displacement->address != sites[i] + 1 || displacement->width != 4) return 4;
    }
    puts("observed 6 scalar requests across 3 known sites");
    return 0;
}

static int check_owned_cosave(const char *dll, const char *snapshot) {
    uint8_t initializers[8];
    if (!owned_storage_read_initializers(snapshot, initializers)) return 2;
    SkseTrialRecordProbe probe = {0};
    owned_storage_probe(dll, initializers, false, &probe);
    owned_storage_expect(probe.output, false, false);
    SksePluginRecordSet *captured = probe.output;
    SksePluginRecordSet *with_integer = owned_storage_add_integer(captured);
    probe = (SkseTrialRecordProbe){.input = with_integer, .callback_work_limit = 250000};
    owned_storage_probe(dll, initializers, true, &probe);
    owned_storage_expect(probe.output, true, true);
    owned_storage_expect(captured, false, false);
    skse_plugin_record_set_close(probe.output);
    skse_plugin_record_set_close(with_integer);
    skse_plugin_record_set_close(captured);

    SksePluginRecordSet *different = NULL;
    assert(skse_plugin_record_set_create(0x424510a3, NULL, 0, &different) == SKSE_SESSION_OK);
    probe = (SkseTrialRecordProbe){.input = different};
    owned_storage_probe(dll, initializers, true, &probe);
    assert(probe.status == SKSE_SESSION_UID_MISMATCH && !probe.output);
    skse_plugin_record_set_close(different);
    puts("Owned StorageUtil restored a signed integer and wrote the current authored mod order.");
    return 0;
}

static int check_owned_leaf(const char *path) {
    uint8_t code[6], scratch[128];
    FILE *source = fopen(path, "rb");
    if (!source) return 2;
    size_t size = fread(code, 1, sizeof code, source);
    const bool exact = size == sizeof code && fgetc(source) == EOF && !ferror(source);
    fclose(source);
    // This assay accepts only a stateless scalar MOV/RET leaf. It neither
    // simulates an engine registry nor makes network or OS calls.
    if (!exact || code[0] != 0xb8 || code[5] != 0xc3) return 2;
    const uint32_t argument = 0; // StaticFunctionTag*.
    for (uint32_t base = 0x10000000; base <= 0x12000000; base += 0x02000000) {
        SksePeImage leaf = {
            .base = base, .size = sizeof code, .bytes = code, .section_count = 1,
            .sections = {{0, sizeof code, SKSE_PE_READ | SKSE_PE_EXEC}}
        };
        SkseX86Result result = skse_x86_call(&leaf, base, (SkseX86Scratch){0x20000000, scratch, sizeof scratch},
                                             &argument, 1, 10, (SkseX86Services){0}, SKSE_X86_CDECL);
        if (result.status != SKSE_X86_OK || result.instructions != 2) return 3;
        printf("GetVersion leaf=%u\n", result.eax);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (argc == 3 && !strcmp(argv[1], "--owned-int-leaf")) return check_owned_leaf(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--owned-cosave")) return check_owned_cosave(argv[2], argv[3]);
    if (argc == 5 && !strcmp(argv[1], "--audit-papyrus")) return audit_owned_papyrus(argv[2], argv[3], argv[4]);
    if (argc == 4 && !strcmp(argv[1], "--owned-papyrus")) return trace_owned_papyrus(argv[2], argv[3], false);
    if (argc == 4 && !strcmp(argv[1], "--observe-papyrus-hooks")) return trace_owned_papyrus(argv[2], argv[3], true);
    if (argc == 3 && !strcmp(argv[1], "--session")) {
        check_session(argv[2]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--session-worker")) {
        check_worker_session(argv[2]);
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        SkseLocalDllReport report;
        SkseProbeFileStatus status = skse_probe_local_dll(argv[i], 4427, &report);
        if (status != SKSE_PROBE_FILE_CHECKED) {
            printf("%u\n", (unsigned)status);
        } else if (report.trial.status == SKSE_TRIAL_COMPLETED) {
            printf("ready,%u,%s", report.trial.plugin_version, report.trial.name);
            if (report.trial.partial_windows_imports) printf(",partial");
            if (report.trial.serialization_callbacks) {
                printf(",%u,%u", report.trial.serialization_callbacks, report.trial.serialization_records);
            }
            if (report.trial.workers_completed) printf(",workers=%u", report.trial.workers_completed);
            if (report.trial.tasks_completed[0] || report.trial.tasks_completed[1])
                printf(",tasks=%u/%u", report.trial.tasks_completed[0], report.trial.tasks_completed[1]);
            putchar('\n');
        } else {
            printf("stopped,%u,%s,%u", (unsigned)report.trial.status,
                   skse_pe_status_text(report.trial.image_status), (unsigned)report.imports.symbol_count);
            if (report.trial.dependencies.import_symbol[0]) {
                printf(",%s,%s", report.trial.dependencies.requested, report.trial.dependencies.import_symbol);
            }
            if (report.trial.load.status != SKSE_X86_OK) printf(",load=%u", (unsigned)report.trial.load.status);
            if (report.trial.partial_windows_imports) {
                const char *function = skse_trial_windows_service_name(report.trial.load.failed_service);
                printf(",partial");
                if (function) printf(",%s", function);
            }
            if (report.trial.task_fault.status != SKSE_X86_OK) printf(",task=%u", (unsigned)report.trial.task_fault.status);
            putchar('\n');
        }
    }
}
