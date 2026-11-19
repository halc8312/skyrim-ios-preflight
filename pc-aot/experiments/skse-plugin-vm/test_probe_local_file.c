/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_local_file.h"
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

int main(int argc, char **argv) {
    if (argc < 2) return 2;
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
            if (report.trial.task_fault.status != SKSE_X86_OK) printf(",task=%u", (unsigned)report.trial.task_fault.status);
            putchar('\n');
        }
    }
}
