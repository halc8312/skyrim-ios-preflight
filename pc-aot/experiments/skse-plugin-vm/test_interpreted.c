/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_trial_cases.h"

int main(int argc, char *argv[]) {
    int clock = argc >= 3 && strcmp(argv[2], "clock") == 0;
    int memory = argc >= 3 && strcmp(argv[2], "memory") == 0;
    int messages = argc >= 3 && strcmp(argv[2], "messages") == 0;
    int moved = argc == 4 && strcmp(argv[3], "move") == 0;
    assert(argc == 2 || ((clock || memory || messages) && (argc == 3 || moved)));
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    uint8_t bytes[64 * 1024];
    size_t length = fread(bytes, 1, sizeof bytes, file);
    int unusable = ferror(file) || !feof(file);
    fclose(file);
    if (unusable) return 2;
    for (size_t i = 0; i < sizeof trial_cases / sizeof trial_cases[0]; ++i) {
        SkseTrialRuntime runtime = {
            .uptime_ms = UINT32_C(0x9100abcd),
            .guest_base = moved ? UINT32_C(0x60000000) : 0,
            .core_messages = (unsigned)messages
        };
        SkseTrialResult result = argc >= 3
            ? skse_plugin_trial_with_runtime(bytes, length, trial_cases[i], runtime)
            : skse_plugin_trial(bytes, length, trial_cases[i]);
        if (result.status != SKSE_TRIAL_COMPLETED && result.status != SKSE_TRIAL_QUERY_DECLINED &&
            result.status != SKSE_TRIAL_LOAD_DECLINED && result.status != SKSE_TRIAL_MESSAGE_FAULT) {
            fprintf(stderr, "case=%zu phase=%u PE=%u x86-query=%u at=0x%08x x86-load=%u at=0x%08x\n",
                    i, (unsigned)result.status, (unsigned)result.image_status,
                    (unsigned)result.query.status, result.query.instruction_va,
                    (unsigned)result.load.status, result.load.instruction_va);
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
        if (memory) {
            for (unsigned service = 3; service <= 5; ++service) assert(result.query.service_calls[service] == 1);
        }
        print_trial(i, query, result.status == SKSE_TRIAL_COMPLETED || result.status == SKSE_TRIAL_MESSAGE_FAULT,
                    result.plugin_version, result.name,
                    result.load.service_calls[0], result.load.service_calls[1]);
        if (messages) {
            unsigned fault = result.status == SKSE_TRIAL_MESSAGE_FAULT;
            if (fault) assert(result.message.status == SKSE_X86_UNSUPPORTED_API);
            printf("messages,%u,%u,%u\n", result.messages_delivered, fault, result.unavailable_interface);
        }
    }
    return 0;
}
