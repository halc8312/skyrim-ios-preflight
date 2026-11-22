/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum {
    CLASS = 0x1100, VTABLE = 0x1200, OBJECTS = 0x1300, NAMES = 0x1400,
    GET_NAME = 0x30000, TABLE_STORAGE = 0x1600, PREFIX_WORDS = 10 + 2 * 2 + 3 + 3 * 2,
    GLOBALS = TABLE_STORAGE + 4 * PREFIX_WORDS, MEMBERS = GLOBALS + 8,
    FIND = 0x1800, STACK = 0x3f00,
    CLASS_FLAGS = 3 | (3 << 2) | (2 << 8),
    GLOBAL_FLAGS = (2 << 20) | (1 << 10) | 2,
    MEMBER_FLAGS = 0xa5a5f004u,
    GUEST_FIXTURE_BEGIN = 0x1000, GUEST_FIXTURE_END = 0x2000
};

uint8_t *MEM;
static unsigned name_calls, compare_calls;

static uint32_t install_function(unsigned index, const char *name)
{
    const uint32_t object = OBJECTS + 8 * index;
    const uint32_t string = NAMES + 16 * index;
    assert(strlen(name) < 16);
    strcpy((char *)MEM + string, name);
    M32(object) = VTABLE;
    M32(object + 4) = string;
    return object;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    const uint32_t object = cpu->r[1];
    assert(target == GET_NAME && M32(cpu->r[4]) == 0xc363db);
    assert(object >= OBJECTS && object < OBJECTS + 8 * 6 && (object - OBJECTS) % 8 == 0);
    assert(M32(object) == VTABLE);
    ++name_calls;
    cpu->r[0] = object + 4; /* Return the address of the fixed-string wrapper. */
    cpu->r[4] += 4;
}

void f_F52591(CPU *cpu)
{
    const uint32_t sp = cpu->r[4];
    assert(M32(sp) == 0xc363e8 && M32(sp + 4) == FIND);
    const uint32_t actual = M32(sp + 8);
    assert(actual >= NAMES && actual < NAMES + 16 * 6 && (actual - NAMES) % 16 == 0);
    ++compare_calls;
    cpu->r[0] = (uint32_t)strcasecmp((const char *)MEM + FIND, (const char *)MEM + actual);
    cpu->r[4] += 4; /* Caller reclaims both raw character pointer arguments. */
}

static void verify_lookup(void (*lookup)(CPU *), uint32_t table, unsigned count,
                          const char *name, uint32_t expected_slot, uint32_t expected_object)
{
    strcpy((char *)MEM + FIND, name);
    const bool direct = lookup == f_C363B0;
    const uint32_t sentinel = 0x12345678;
    M32(STACK) = sentinel;
    M32(STACK + 4) = direct ? table : FIND;
    M32(STACK + 8) = direct ? count : sentinel;
    M32(STACK + 12) = direct ? (count ? FIND : UINT32_MAX) : sentinel;
    M32(STACK + 16) = sentinel;
    uint32_t stack[5];
    uint8_t fixture[GUEST_FIXTURE_END - GUEST_FIXTURE_BEGIN];
    memcpy(stack, MEM + STACK, sizeof stack);
    memcpy(fixture, MEM + GUEST_FIXTURE_BEGIN, sizeof fixture);

    CPU cpu = {.r = {0xaaaa, CLASS, 0xbbbb, 0xcccc, STACK, 0xdddd, 0xeeee, 0xffff}};
    name_calls = compare_calls = 0;
    lookup(&cpu);

    assert(cpu.r[0] == expected_slot && cpu.r[4] == STACK + (direct ? 16 : 8));
    assert(cpu.r[3] == 0xcccc && cpu.r[5] == 0xdddd && cpu.r[6] == 0xeeee && cpu.r[7] == 0xffff);
    assert(!memcmp(stack, MEM + STACK, sizeof stack));
    assert(!memcmp(fixture, MEM + GUEST_FIXTURE_BEGIN, sizeof fixture));
    assert(name_calls == compare_calls && (name_calls != 0) == (count != 0));
    if (expected_slot)
        assert(M32(expected_slot) == expected_object && expected_slot != expected_object);
}

int main(void)
{
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    assert(gm_commit_page(&space, CLASS / space.page_size * space.page_size, NULL));
    if (!gm_pointer(&space, STACK, 20))
        assert(gm_commit_page(&space, STACK / space.page_size * space.page_size, NULL));
    MEM = space.base;
    M32(VTABLE + 4) = GET_NAME;
    M32(CLASS + 0x10) = CLASS_FLAGS;
    M32(CLASS + 0x14) = GLOBAL_FLAGS;
    M32(CLASS + 0x18) = MEMBER_FLAGS;
    M32(CLASS + 0x1c) = TABLE_STORAGE;
    for (unsigned i = 0; i < PREFIX_WORDS; ++i) M32(TABLE_STORAGE + i * 4) = UINT32_MAX;

    const uint32_t keep = install_function(0, "KeepUpdate");
    const uint32_t stop = install_function(1, "StopUpdate");
    const uint32_t move_down = install_function(2, "MoveDown");
    const uint32_t move_up = install_function(3, "MoveUp");
    const uint32_t on_update = install_function(4, "OnUpdate");
    const uint32_t member_stop = install_function(5, "StopUpdate");
    M32(GLOBALS) = keep;
    M32(GLOBALS + 4) = stop;
    M32(MEMBERS) = move_down;
    M32(MEMBERS + 4) = move_up;
    M32(MEMBERS + 8) = on_update;
    M32(MEMBERS + 12) = member_stop;

    verify_lookup(f_C36540, 0, 2, "keepupdate", GLOBALS, keep);
    verify_lookup(f_C36540, 0, 2, "StopUpdate", GLOBALS + 4, stop);
    verify_lookup(f_C365A0, 0, 4, "MoveDown", MEMBERS, move_down);
    verify_lookup(f_C365A0, 0, 4, "MoveUp", MEMBERS + 4, move_up);
    verify_lookup(f_C365A0, 0, 4, "OnUpdate", MEMBERS + 8, on_update);
    verify_lookup(f_C365A0, 0, 4, "StopUpdate", MEMBERS + 12, member_stop);
    verify_lookup(f_C36540, 0, 2, "MoveUp", 0, 0);
    verify_lookup(f_C365A0, 0, 4, "KeepUpdate", 0, 0);
    verify_lookup(f_C363B0, MEMBERS, 4, "OnUpdate", MEMBERS + 8, on_update);
    verify_lookup(f_C363B0, UINT32_MAX, 0, "OnUpdate", 0, 0);

    M32(CLASS + 0x14) &= ~(0x1ffu << 20);
    M32(CLASS + 0x18) &= ~0x7ffu;
    M32(CLASS + 0x1c) = 0;
    verify_lookup(f_C36540, 0, 0, "KeepUpdate", 0, 0);
    verify_lookup(f_C365A0, 0, 0, "MoveUp", 0, 0);
    assert(gm_release(&space, NULL));
    puts("Classic class lookups return slots from separate global and member tables");
    return 0;
}
