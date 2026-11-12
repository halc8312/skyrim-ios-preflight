/* SPDX-License-Identifier: GPL-3.0-only */
#include "ios_runtime.h"
#include <stdio.h>
#include <time.h>

void skyrim_ios_stage(const char *stage) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "IOS_STAGE %s monotonic=%lld.%09ld\n", stage,
            (long long)now.tv_sec, now.tv_nsec);
    fflush(stderr);
}

void skyrim_ios_present_result(int32_t result) {
    static unsigned successful_presents;
    static int32_t last_error;
    if (result < 0) {
        if (result != __atomic_exchange_n(&last_error, result, __ATOMIC_RELAXED))
            fprintf(stderr, "IOS_PRESENT failure=0x%08x\n", (uint32_t)result);
        fflush(stderr);
        return;
    }
    __atomic_store_n(&last_error, 0, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&successful_presents, 1, __ATOMIC_RELAXED) == 1)
        skyrim_ios_stage("first-successful-present-title-not-verified");
}
