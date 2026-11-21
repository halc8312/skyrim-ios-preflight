/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    STACK = 0x2fe0, MENU = 0x2100, ROWS = 0x2260, REQUESTS = 0x2400,
    FOLLOWUP_TARGET = 0x2500, FOLLOWUP_SLOT = 0x1b10d5c, CACHE_ARRAYS = 0x1b3fd30,
    PLAYER_SLOT = 0x1b2e8e4,
    ARRAY_COUNT = 3, ARRAY_BYTES = 12, REQUEST_BYTES = 32,
    IN_PROGRESS = 0x10000000, OTHER_PENDING = 0x20000000, READY = 0x40000000,
};

uint8_t *MEM;
static bool skip_initial_warm;
static unsigned setup_calls, warm_calls, followup_calls, clear_calls;
static uint32_t expected_warm_return = 0x886ad0;

void skyrim_chargen_probe_open_calls(CPU *cpu);
void f_8868C0(CPU *cpu);
void f_87EAA0(CPU *cpu);
void f_886B50(CPU *cpu);

static void return_guest(CPU *cpu, unsigned arguments) {
    cpu->r[4] += 4 * (arguments + 1);
}

static void set_request(unsigned array, uint32_t state) {
    uint32_t descriptor = CACHE_ARRAYS + ARRAY_BYTES * array;
    uint32_t row = ROWS + 4 * array;
    uint32_t request = REQUESTS + REQUEST_BYTES * array;
    M32(descriptor) = row;
    M32(descriptor + 8) = 1;
    M32(row) = request;
    M32(request + 0x10) = state;
}

bool skyrim_chargen_probe_skip_open_warm(void) {
    return skip_initial_warm;
}

void f_880EA0(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x886acb && cpu->r[6] == MENU);
    assert(setup_calls == followup_calls);
    ++setup_calls;
    return_guest(cpu, 0);
}

void f_44D240(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x8868d2 && M32(cpu->r[4] + 16u) == expected_warm_return);
    ++warm_calls;
    // Record entry into the original routine and stop before engine traversal.
    set_request(0, IN_PROGRESS);
    cpu->r[0] = 0;
    return_guest(cpu, 0);
}

void f_55B510(CPU *cpu) { (void)cpu; abort(); }
void f_8867A0(CPU *cpu) { (void)cpu; abort(); }
void f_8816A0(CPU *cpu) { (void)cpu; abort(); }
void f_881BF0(CPU *cpu) { (void)cpu; abort(); }

void f_878DB0(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x886adf && M32(cpu->r[4] + 4) == MENU + 0x1c);
    assert(cpu->r[1] == FOLLOWUP_TARGET && setup_calls == followup_calls + 1);
    ++followup_calls;
    return_guest(cpu, 1);
}

static void clear_array(CPU *cpu, unsigned array) {
    uint32_t descriptor = CACHE_ARRAYS + ARRAY_BYTES * array;
    assert(cpu->r[1] == descriptor && M32(cpu->r[4] + 4) == 0);
    assert(M32(cpu->r[4] + 8) == M32(descriptor + 8));
    ++clear_calls;
    return_guest(cpu, 2);
}

void f_579CA0(CPU *cpu) { clear_array(cpu, 0); }
void f_885C50(CPU *cpu) { clear_array(cpu, 1); }
void f_886800(CPU *cpu) { clear_array(cpu, 2); }

static CPU menu_cpu(void) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    cpu.r[6] = MENU;
    return cpu;
}

static bool has_ready_cache(void) {
    CPU cpu = menu_cpu();
    f_87EAA0(&cpu);
    assert(cpu.r[4] == STACK + 4 && cpu.r[6] == MENU);
    assert((uint8_t)cpu.r[0] <= 1);
    return (uint8_t)cpu.r[0] != 0;
}

static void check_warm_callers(void) {
    const uint32_t callers[] = {0x886ad0u, 0x887069u, 0x887233u};
    for (size_t i = 0; i < sizeof callers / sizeof callers[0]; ++i) {
        const unsigned before = warm_calls;
        expected_warm_return = callers[i];
        M32(STACK) = expected_warm_return;
        M32(STACK - 4u) = 0x315dc0deu;
        CPU cpu = menu_cpu();
        cpu.r[0] = 0x12345678u;
        cpu.r[1] = 0x89abcdefu;
        cpu.flags = (Flags){.cf = 1, .zf = 1, .of = 1};
        CPU original = cpu;

        skip_initial_warm = true;
        f_8868C0(&cpu);
        assert(cpu.r[4] == STACK + 4u && warm_calls == before);
        cpu.r[4] = STACK;
        assert(!memcmp(cpu.r, original.r, sizeof cpu.r) &&
               !memcmp(&cpu.flags, &original.flags, sizeof cpu.flags));
        assert(M32(STACK) == expected_warm_return && M32(STACK - 4u) == 0x315dc0deu);

        skip_initial_warm = false;
        f_8868C0(&cpu);
        assert(cpu.r[4] == STACK + 4u && warm_calls == before + 1);
    }
}

int main(void) {
    gm_space memory = {0};
    assert(gm_reserve(&memory, NULL));
    const uint32_t ranges[] = {MENU, FOLLOWUP_SLOT, CACHE_ARRAYS, PLAYER_SLOT};
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i)
        assert(gm_commit_page(&memory, (ranges[i] / memory.page_size) * memory.page_size, NULL));
    MEM = memory.base;
    M32(FOLLOWUP_SLOT) = FOLLOWUP_TARGET;
    M32(PLAYER_SLOT) = MENU;

    assert(has_ready_cache());
    CPU cpu = menu_cpu();
    skip_initial_warm = true;
    skyrim_chargen_probe_open_calls(&cpu);
    assert(cpu.r[4] == STACK && setup_calls == 1 && warm_calls == 0 && followup_calls == 1);
    assert(has_ready_cache());

    cpu = menu_cpu();
    skip_initial_warm = false;
    skyrim_chargen_probe_open_calls(&cpu);
    assert(cpu.r[4] == STACK && setup_calls == 2 && warm_calls == 1 && followup_calls == 2);
    assert(!has_ready_cache());

    cpu = menu_cpu();
    skip_initial_warm = true;
    skyrim_chargen_probe_open_calls(&cpu);
    assert(cpu.r[4] == STACK && setup_calls == 3 && warm_calls == 1 && followup_calls == 3);
    assert(!has_ready_cache()); // A request already in flight still participates in readiness.
    check_warm_callers();

    for (unsigned i = 0; i < ARRAY_COUNT; ++i) set_request(i, READY);
    assert(has_ready_cache());
    for (unsigned i = 0; i < ARRAY_COUNT; ++i) {
        set_request(i, IN_PROGRESS);
        assert(!has_ready_cache());
        set_request(i, OTHER_PENDING);
        assert(!has_ready_cache());
        set_request(i, READY);
        assert(has_ready_cache());
    }
    cpu = menu_cpu();
    f_886B50(&cpu);
    assert(cpu.r[4] == STACK + 4 && clear_calls == ARRAY_COUNT);
    for (unsigned i = 0; i < ARRAY_COUNT; ++i)
        assert(M32(CACHE_ARRAYS + ARRAY_BYTES * i + 8) == 0);
    assert(has_ready_cache());

    assert(gm_release(&memory, NULL));
    puts("Reviewed LE warm entry preserved guest state and original cache readiness.");
    return 0;
}
