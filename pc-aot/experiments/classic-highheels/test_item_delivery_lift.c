/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STACK = 0x2fc0, EVENT = 0x2100, ACTOR = 0x2200, OWNER = 0x2300, TEBCOPY = 0x3000, TLS = 0x3400,
    TLS_SLOT = 0x1bbeb54, NAME_MASK = 0x1b40688, VM = 0x2500, VM_TABLE = 0x2600, HANDLE_TABLE = 0x2700,
    ADD_REF_IMPORT = 0x106b150, RELEASE_IMPORT = 0x106b154,
    EQUIP_ITEM_NAME = 0x10eb368, UNEQUIP_ITEM_NAME = 0x10eb358,
    EQUIP_ACTOR_NAME = 0x10eb388, UNEQUIP_ACTOR_NAME = 0x10eb374,
    MAKE_HANDLE = 1, SEND_EVENT, ADD_REF, RELEASE
};

uint8_t *MEM;
static bool equip, item_dispatch;
static unsigned actor_events, item_events, notifications;
static const CPU *invoker;

void f_8C9910(CPU *cpu);
void unobserved_item_delivery(CPU *cpu);

void f_43BA00(CPU *cpu)
{
    M32(cpu->r[1]) = M32(cpu->r[4] + 4);
    cpu->r[4] += 8;
}

void f_F520BC(CPU *cpu)
{
    cpu->r[4] += 4;
}

void f_A50D50(CPU *cpu)
{
    M32(cpu->r[1]) = M32(M32(cpu->r[4] + 4));
    cpu->r[4] += 8;
}

void f_451A30(CPU *cpu)
{
    cpu->r[0] = 0;
    cpu->r[4] += 4;
}

void f_8B39D0(CPU *cpu)
{
    assert(M32(cpu->r[4]) == 0x8c9b1b);
    cpu->r[0] = 0x4225;
    cpu->r[4] += 4;
}

void f_8C6B30(CPU *cpu)
{
    assert(M32(cpu->r[4]) == 0x8c9a65);
    cpu->r[4] += 24;
}

void f_C332D0(CPU *cpu)
{
    cpu->r[4] += 4;
}

void f_A511B0(CPU *cpu)
{
    M32(cpu->r[1]) = 0;
    cpu->r[4] += 4;
}

void f_8C5EF0(CPU *cpu)
{
    /* The authored test selects the unique item slot's primary delivery. */
    assert(0 && "unexpected alternate item delivery");
    cpu->r[4] += 4;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    uint32_t sp = cpu->r[4];
    switch (target) {
    case MAKE_HANDLE:
        assert(cpu->r[1] == OWNER + 0x184 && M32(sp + 8) == ACTOR);
        cpu->r[0] = 0x1225;
        cpu->r[2] = 0;
        cpu->r[4] += 12;
        break;
    case SEND_EVENT: {
        assert(cpu->r[1] == VM);
        uint32_t name = M32(M32(sp + 12));
        if (M32(sp) == 0x8c9a50) {
            assert(name == (equip ? EQUIP_ACTOR_NAME : UNEQUIP_ACTOR_NAME));
            ++actor_events;
        } else {
            assert(M32(sp) == 0x8c9b6a && item_dispatch);
            assert(name == (equip ? EQUIP_ITEM_NAME : UNEQUIP_ITEM_NAME));
            assert(M32(M32(sp + 16) + 4) == ACTOR && M32(ACTOR + 0x18) == 8);
            /* This authored callee must not change the already captured phase. */
            M8(EVENT + 0xe) = !equip;
            ++item_events;
        }
        cpu->r[4] += 20;
        break;
    }
    case ADD_REF:
        assert(M32(sp) == 0x8c9b4a && M32(sp + 4) == ACTOR + 0x18);
        cpu->r[0] = ++M32(ACTOR + 0x18);
        cpu->r[4] += 8;
        break;
    case RELEASE:
        assert(M32(sp) == 0x8c9b7f && M32(sp + 4) == ACTOR + 0x18);
        cpu->r[0] = --M32(ACTOR + 0x18);
        cpu->r[4] += 8;
        break;
    default:
        assert(0 && "unexpected virtual or imported call");
    }
}

void highheels_observe_item_delivery(const CPU *cpu, uint8_t phase)
{
    assert(cpu == invoker && cpu->r[6] == EVENT);
    assert(actor_events == 1 && item_events == 1 && item_dispatch);
    assert(M32(ACTOR + 0x18) == 8);
    assert(phase == (equip ? 1u : 0u));
    ++notifications;
}

typedef struct Result {
    uint32_t registers[8];
    Flags flags;
    uint32_t tls_value;
    uint32_t ref_count;
    uint32_t name_mask;
    uint8_t event_phase;
    uint8_t stack_bytes[112];
} Result;

static Result run(void (*dispatch)(CPU *), bool equipped, bool unique_id, bool observe)
{
    CPU cpu = {.r = {1, OWNER, 3, 4, STACK, 5, 6, 7}, .fs_base = TEBCOPY};
    memset(MEM + STACK - 100, 0, 112);
    M32(STACK) = 0x100;
    M32(STACK + 4) = EVENT;
    M32(STACK + 8) = 0x99;
    M32(EVENT) = ACTOR;
    M32(EVENT + 4) = 0x111;
    M32(EVENT + 8) = 0;
    M16(EVENT + 0xc) = unique_id ? 1 : 0;
    M8(EVENT + 0xe) = equipped;
    M32(TLS + 0x4ac) = 0x42;
    M32(ACTOR + 0x18) = 7;
    M32(NAME_MASK) = 0;
    equip = equipped;
    item_dispatch = unique_id;
    actor_events = item_events = notifications = 0;
    invoker = &cpu;
    dispatch(&cpu);
    invoker = NULL;
    assert(actor_events == 1 && item_events == (unsigned)item_dispatch);
    assert(notifications == (unsigned)(observe && item_dispatch));
    assert(cpu.r[4] == STACK + 12 && M32(ACTOR + 0x18) == 7 && M32(TLS + 0x4ac) == 0x42);
    Result result = {.flags = cpu.flags, .tls_value = M32(TLS + 0x4ac),
                     .ref_count = M32(ACTOR + 0x18), .name_mask = M32(NAME_MASK),
                     .event_phase = M8(EVENT + 0xe)};
    memcpy(result.registers, cpu.r, sizeof result.registers);
    memcpy(result.stack_bytes, MEM + STACK - 100, sizeof result.stack_bytes);
    return result;
}

int main(void)
{
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    const uint32_t addresses[] = {STACK, TEBCOPY, TLS_SLOT, NAME_MASK, ADD_REF_IMPORT};
    for (unsigned i = 0; i < sizeof addresses / sizeof addresses[0]; ++i) {
        uint32_t addr = addresses[i];
        if (!gm_pointer(&space, addr, 4))
            assert(gm_commit_page(&space, addr / space.page_size * space.page_size, NULL));
    }
    MEM = space.base;
    M32(TEBCOPY + 0x2c) = TEBCOPY + 0x100;
    M32(TEBCOPY + 0x100) = TLS;
    M32(TLS_SLOT) = 0;
    M32(OWNER + 0x184) = HANDLE_TABLE;
    M32(HANDLE_TABLE + 0x10) = MAKE_HANDLE;
    M32(OWNER + 0xcc) = VM;
    M32(VM) = VM_TABLE;
    M32(VM_TABLE + 0x8c) = SEND_EVENT;
    M8(ACTOR + 0x12) = 0x3e;
    M32(ACTOR + 0xc) = 0x12345;
    M32(ADD_REF_IMPORT) = ADD_REF;
    M32(RELEASE_IMPORT) = RELEASE;
    for (unsigned equipped = 0; equipped <= 1; ++equipped) {
        for (unsigned unique = 0; unique <= 1; ++unique) {
            Result a = run(unobserved_item_delivery, equipped, unique, false);
            Result b = run(f_8C9910, equipped, unique, true);
            assert(!memcmp(a.registers, b.registers, sizeof a.registers));
            assert(!memcmp(&a.flags, &b.flags, sizeof a.flags));
            assert(!memcmp(a.stack_bytes, b.stack_bytes, sizeof a.stack_bytes));
            assert(a.tls_value == b.tls_value && a.ref_count == b.ref_count && a.name_mask == b.name_mask);
            assert(a.event_phase == b.event_phase);
        }
    }
    assert(gm_release(&space, NULL));
    puts("Classic primary item equip and unequip delivery preserves the borrowed reference and guest state");
    return 0;
}
