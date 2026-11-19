/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "plugin_trial.h"

#define USE_DEFAULT_HANDLE UINT32_C(59)
#define INVALID_HANDLE UINT32_MAX
static const SkseTrialOptions trial_cases[] = {
    {0x01070030u, 0x09200000u, 0, 48, USE_DEFAULT_HANDLE, 1, 1},
    {0x01070020u, 0x09200000u, 0, 48, USE_DEFAULT_HANDLE, 1, 1},
    {0x01070030u, 0x091d0000u, 0, 48, USE_DEFAULT_HANDLE, 1, 1},
    {0x01070030u, 0x09200000u, 1, 48, USE_DEFAULT_HANDLE, 1, 1},
    {0x01070030u, 0x09200000u, 0, 47, USE_DEFAULT_HANDLE, 1, 1},
    {0x01070030u, 0x09200000u, 0, 48, INVALID_HANDLE, 1, 1},
    {0x01070030u, 0x09200000u, 0, 48, USE_DEFAULT_HANDLE, 0, 1},
    {0x01070030u, 0x09200000u, 0, 48, USE_DEFAULT_HANDLE, 1, 0}
};

static void print_trial(size_t index, unsigned query, unsigned load,
                        uint32_t version, const char *name, unsigned release, unsigned handle) {
    printf("%zu,%u,%u,%u,%s,%u,%u\n", index, query, load, version, name, release, handle);
}
