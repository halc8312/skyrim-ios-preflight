/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_trial_cases.h"

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

int main(int argc, char *argv[]) {
    int clock = argc >= 3 && strcmp(argv[2], "clock") == 0;
    int memory = argc >= 3 && strcmp(argv[2], "memory") == 0;
    int allocation = argc >= 3 && strcmp(argv[2], "allocation") == 0;
    int interlocked = argc >= 3 && (!strcmp(argv[2], "interlocked") || !strcmp(argv[2], "locked-instructions"));
    int module_lookup = argc >= 3 && strcmp(argv[2], "module-lookup") == 0;
    int file_assets = argc >= 3 && strcmp(argv[2], "file-assets") == 0;
    int events = argc >= 3 && strcmp(argv[2], "events") == 0;
    int worker = argc >= 3 && strcmp(argv[2], "worker") == 0;
    int tls = argc >= 3 && strcmp(argv[2], "tls") == 0;
    int tls_pair = argc >= 4 && strcmp(argv[2], "static-tls-pair") == 0;
    int static_tls = tls_pair || (argc >= 3 && strcmp(argv[2], "static-tls") == 0);
    int messages = argc >= 3 && strcmp(argv[2], "messages") == 0;
    int serialization = argc >= 3 && strcmp(argv[2], "serialization") == 0;
    int dependency = tls_pair || (argc >= 4 && strcmp(argv[2], "dependency") == 0);
    int moved = argc >= 4 && strcmp(argv[argc - 1], "move") == 0;
    assert(argc == 2 || dependency ||
        ((clock || memory || allocation || interlocked || module_lookup || file_assets || events || tls || static_tls ||
          messages || serialization || worker) &&
         (argc == 3 || moved)));
    uint8_t bytes[64 * 1024];
    size_t length = read_owned_dll(argv[1], bytes, sizeof bytes);
    if (!length) return 2;
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
                       .attributes = file_assets ? authored_attributes : NULL}
        };
        SkseTrialResult result = argc >= 3
            ? skse_plugin_trial_with_runtime(bytes, length, trial_cases[i], runtime)
            : skse_plugin_trial(bytes, length, trial_cases[i]);
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
        if (query) assert(result.load.instructions && result.load.instructions < 1000);
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
    }
    return 0;
}
