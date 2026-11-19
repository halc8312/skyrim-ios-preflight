/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <inttypes.h>
#include <stdio.h>

enum { NO_DEVICE = 1, NO_OUTPUT = 2 };
struct DepthCase {
    uint32_t depth, hr, flags;
};
static const struct DepthCase cases[] = {{0, 0, 0},
                                         {1, 0, 0},
                                         {2, 0, 0},
                                         {UINT32_C(0x82345678), 1, 0},
                                         {1, UINT32_C(0x88760868), 0},
                                         {1, 0, NO_OUTPUT},
                                         {1, 0, NO_DEVICE}};
#define CASES (sizeof(cases) / sizeof(*cases))
#define INITIAL_WORD UINT32_C(0xa5b6c7d8)
static void report(unsigned id, uint32_t hr, uint32_t depth, uint32_t refs, const char* trace) {
    printf("%u %08" PRIx32 " %08" PRIx32 " %" PRIu32 " %s\n", id, hr, depth, refs, trace);
}
