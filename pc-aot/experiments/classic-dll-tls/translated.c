/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile-time targets and private, bounded storage for the authored image. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "funcs_ownedtls.h"
#include "rt_table.h"
#include "owned_tls_layout.h"
#include "trace.h"

enum {
    THREAD_BASE = 0x1000, THREAD_STRIDE = 0x100, TLS_VECTOR = 0x40, TLS_BLOCK = 0x80,
    LOADER_TLS_ARRAY = 0x2c, MODULE_SLOTS = 8, STACK = 0x3400, OUTPUT = 0x4000,
};
static uint8_t arena[OWNED_TLS_IMAGE_END];
uint8_t *MEM = arena;
static unsigned module_slot;
static int live[2];
static const uint32_t marker = 0x91aabbcc;

static void check(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "%s\n", message);
        exit(2);
    }
}

void rt_call(CPU *cpu, uint32_t target) {
    (void)cpu;
    (void)target;
    check(0, "The closed TLS fixture has no indirect guest calls or Windows API calls");
}
void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    rt_call(cpu, target);
}

static uint32_t offsets(unsigned thread) {
    check(thread < 2, "Unknown synthetic thread");
    return THREAD_BASE + thread * THREAD_STRIDE;
}

static void allocate_tls(unsigned thread) {
    uint32_t teb = offsets(thread), vector = teb + TLS_VECTOR, block = teb + TLS_BLOCK;
    _Static_assert(TLS_VECTOR + MODULE_SLOTS * 4 <= TLS_BLOCK, "Synthetic vector overlaps TLS");
    _Static_assert(TLS_BLOCK + sizeof owned_template + OWNED_TLS_ZERO <= THREAD_STRIDE,
                   "Synthetic TLS area overlaps next thread");
    check(!live[thread] && !M32(teb + LOADER_TLS_ARRAY) && module_slot < MODULE_SLOTS, "TLS already active");
    check(OWNED_TLS_ALIGN == 4 && !(block % OWNED_TLS_ALIGN), "Bad authored alignment");
    memset(MEM + block, 0x9d, sizeof owned_template + OWNED_TLS_ZERO);
    memcpy(MEM + block, owned_template, sizeof owned_template);
    memset(MEM + block + sizeof owned_template, 0, OWNED_TLS_ZERO);
    M32(vector + module_slot * 4) = block;
    M32(teb + LOADER_TLS_ARRAY) = vector;
    live[thread] = 1;
    for (size_t i = 0; i < OWNED_TLS_ZERO; ++i)
        check(M8(block + sizeof owned_template + i) == 0, "Virtual zero-fill failed");
}

static void release_tls(unsigned thread) {
    uint32_t teb = offsets(thread);
    check(live[thread] && M32(teb + LOADER_TLS_ARRAY) == teb + TLS_VECTOR &&
              M32(teb + TLS_VECTOR + module_slot * 4) == teb + TLS_BLOCK, "Wrong TLS owner");
    memset(MEM + teb, 0, THREAD_STRIDE);
    live[thread] = 0;
}

typedef void (*TranslatedFunction)(CPU *);
static uint32_t invoke(unsigned thread, TranslatedFunction fn, unsigned pop,
                       uint32_t a, uint32_t b, uint32_t c) {
    CPU cpu = {0};
    check(thread < 2 && live[thread] && (pop == 0 || pop == 12), "Call on unavailable TLS");
    cpu.fs_base = offsets(thread);
    cpu.r[3] = 0x33333333;
    cpu.r[5] = 0x55555555;
    cpu.r[6] = 0x66666666;
    cpu.r[7] = 0x77777777;
    cpu.r[4] = STACK;
    M32(STACK) = marker;
    M32(STACK + 4) = a;
    M32(STACK + 8) = b;
    M32(STACK + 12) = c;
    fn(&cpu);
    check(cpu.r[4] == STACK + 4 + pop && M32(STACK) == marker && cpu.fs_base == offsets(thread) &&
          !cpu.thread && cpu.r[3] == 0x33333333 && cpu.r[5] == 0x55555555 &&
          cpu.r[6] == 0x66666666 && cpu.r[7] == 0x77777777, "Guest TLS/calling convention changed");
    return cpu.r[0];
}

static void notify(unsigned thread, uint32_t reason) {
    invoke(thread, OWNED_TLS_CALLBACK_A, 12, OWNED_TLS_BASE, reason, 0);
    invoke(thread, OWNED_TLS_CALLBACK_B, 12, OWNED_TLS_BASE, reason, 0);
    check(invoke(thread, OWNED_TLS_ENTRY, 12, OWNED_TLS_BASE, reason, 0) == 1, "DllMain reported failure");
}

static void run_case(unsigned number, unsigned thread) {
    uint32_t value = (number + 1u) * 0x11111111u, count = (number + 1u) * 4;
    uint32_t before = invoke(thread, OWNED_TLS_VALUE, 0, 0, 0, 0);
    uint32_t before_count = invoke(thread, OWNED_TLS_COUNT, 0, 0, 0, 0);
    uint32_t returned = invoke(thread, OWNED_TLS_SET, 0, value, count, 0);
    uint32_t after = invoke(thread, OWNED_TLS_VALUE, 0, 0, 0, 0);
    uint32_t after_count = invoke(thread, OWNED_TLS_COUNT, 0, 0, 0, 0);
    printf("context %u %08" PRIx32 " %" PRIu32 " %08" PRIx32 " %08" PRIx32 " %" PRIu32 "\n",
           number, before, before_count, returned, after, after_count);
}

int main(int argc, char **argv) {
    check(argc == 2 && (strcmp(argv[1], "0") == 0 || strcmp(argv[1], "5") == 0), "Select test slot 0 or 5");
    module_slot = argv[1][0] - '0';
    memcpy(MEM + OWNED_TLS_DATA, owned_data, sizeof owned_data);
    check(M32(OWNED_TLS_INDEX) == UINT32_MAX, "The authored unassigned slot sentinel changed");
    M32(OWNED_TLS_INDEX) = module_slot;

    allocate_tls(0);
    notify(0, 1); /* Explicit LoadLibrary on the loading thread. */
    invoke(0, OWNED_TLS_BIND, 0, OUTPUT, 0, 0);
    run_case(0, 0);
    for (unsigned i = 1; i <= 2; ++i) {
        allocate_tls(1); /* Reuse the same synthetic storage for a fresh worker. */
        notify(1, 2);
        run_case(i, 1);
        notify(1, 3);
        release_tls(1);
    }
    uint32_t value = invoke(0, OWNED_TLS_VALUE, 0, 0, 0, 0);
    uint32_t count = invoke(0, OWNED_TLS_COUNT, 0, 0, 0, 0);
    printf("loading context after threads %08" PRIx32 " %" PRIu32 "\n", value, count);
    notify(0, 0); /* Explicit FreeLibrary with no other live workers. */
    release_tls(0);
    uint32_t used = M32(OUTPUT);
    check(used == 18 && used <= TLS_TEST_EVENTS, "Missing or extra TLS notifications");
    for (uint32_t i = 0; i < used; ++i) {
        uint32_t row = OUTPUT + 4 + i * 16;
        printf("notify %" PRIu32 " %" PRIu32 " %08" PRIx32 " %" PRIu32 "\n",
               M32(row), M32(row + 4), M32(row + 8), M32(row + 12));
    }
    return 0;
}
