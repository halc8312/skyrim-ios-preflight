/* SPDX-License-Identifier: GPL-3.0-only
 * Authored host fixture. A one-byte file models success/failure, independent
 * of the game save format. The fixture never opens it for writing. */
#include "lifecycle.h"
#include <inttypes.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void empty_call(void) {}
static jmp_buf exit_site;
static void leave_nonlocally(void) { longjmp(exit_site, 1); }
static void interrupted_case(void) {
    if (!setjmp(exit_site))
        SKYRIM_LIFECYCLE_CALL(SkyrimLoadSlot, leave_nonlocally(), SkyrimReturnedTrue);
    SKYRIM_LIFECYCLE_CALL(SkyrimRevertForReload, empty_call(), SkyrimNoResult);
}
static uint32_t boolean_result(int result) { return result ? SkyrimReturnedTrue : SkyrimReturnedFalse; }
static int save_vm(int value) { return value; }
static int load_vm(int value) { return value; }
static void save_slot(int value) {
    int result;
    SKYRIM_LIFECYCLE_CALL(SkyrimSaveVM, result = save_vm(value), boolean_result(result));
    (void)result;
}
static int load_slot(int value) {
    SKYRIM_LIFECYCLE_CALL(SkyrimRevertForLoad, empty_call(), SkyrimNoResult);
    int result;
    SKYRIM_LIFECYCLE_CALL(SkyrimLoadVM, result = load_vm(value), boolean_result(result));
    return result;
}
static _Atomic unsigned arrivals;
static void parallel_inner(void) {
    atomic_fetch_add(&arrivals, 1);
    while (atomic_load(&arrivals) < 2) {}
    SKYRIM_LIFECYCLE_CALL(SkyrimSaveVM, empty_call(), SkyrimReturnedTrue);
}
static void *parallel_run(void *unused) {
    (void)unused;
    SKYRIM_LIFECYCLE_CALL(SkyrimSaveSlot, parallel_inner(), SkyrimNoResult);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    int result = 1;
    if (!strcmp(argv[1], "overflow")) {
        for (int i = 0; i < 300; ++i)
            SKYRIM_LIFECYCLE_CALL(SkyrimRegisterPapyrus, empty_call(), SkyrimNoResult);
    } else if (!strcmp(argv[1], "nonlocal")) {
        interrupted_case();
    } else if (!strcmp(argv[1], "threads")) {
        pthread_t threads[2];
        for (int i = 0; i < 2; ++i) if (pthread_create(&threads[i], NULL, parallel_run, NULL)) return 1;
        for (int i = 0; i < 2; ++i) if (pthread_join(threads[i], NULL)) return 1;
    } else {
        FILE *input = fopen(argv[1], "rb");
        if (!input) return 1;
        result = fgetc(input) == '1';
        if (fclose(input)) return 1;
        SKYRIM_LIFECYCLE_CALL(SkyrimRegisterPapyrus, empty_call(), SkyrimNoResult);
        SKYRIM_LIFECYCLE_CALL(SkyrimSaveSlot, save_slot(result), SkyrimNoResult);
        SKYRIM_LIFECYCLE_CALL(SkyrimLoadSlot, result = load_slot(result), boolean_result(result));
        SKYRIM_LIFECYCLE_CALL(SkyrimRevertForReload, empty_call(), SkyrimNoResult);
    }
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    uint64_t dropped = 0;
#if defined(SKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE) && SKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE
    size_t count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &dropped);
#else
    size_t count = 0;
    (void)boolean_result;
#endif
    printf("%d %zu %" PRIu64 "\n", result, count, dropped);
    for (size_t i = 0; i < count; ++i)
        printf("%u %u %u %u %u\n", rows[i].site, rows[i].returning, rows[i].outcome, rows[i].ticket, rows[i].parent);
    return 0;
}
