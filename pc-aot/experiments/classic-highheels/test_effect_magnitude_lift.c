/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STACK = 0x2f00, POLICY = 0x2100, POLICY_TABLE = 0x2180, ACTOR = 0x2200,
    TARGET_TABLE = 0x2300, FIRST_NODE = 0x2380, SECOND_NODE = 0x2390,
    DECOY = 0x2400, EFFECT = 0x2480,
    ACTIVE_EFFECT_TYPE = 0x8b, ACTOR_FORM_ID = 0x17345, EFFECT_NUMBER = 0x42,
    FIND_EFFECT_TABLE = 0x10ea224, VISITOR_ADDRESS = 0x8b3b00, IS_TYPE_ADDRESS = 0x8b3b20,
    HOST_INVALID_HANDLE = 1, HOST_GET_EFFECT_LIST = 2,
    MAGNITUDE_BITS = 0x40b00000
};

uint8_t *MEM;
static bool observe, have_actor, include_effect;
static unsigned actor_lookups, list_reads, visits, samples;
static uint32_t magnitude;
static const CPU *invoker;

void f_8B39F0(CPU *);
void f_8B3B00(CPU *);
void f_8B3B20(CPU *);
void f_8B3E30(CPU *);
void observed_effect_visitor(CPU *);

/* These tests run only the ActiveMagicEffect branch of the original policy. */
static void unreachable(void)
{
    assert(0 && "unexpected branch outside the active effect lookup");
}

void f_8B3900(CPU *cpu) { (void)cpu; unreachable(); }
void f_8B3980(CPU *cpu) { (void)cpu; unreachable(); }
void f_8B3A50(CPU *cpu) { (void)cpu; unreachable(); }
void f_5509E0(CPU *cpu) { (void)cpu; unreachable(); }
void f_56B6C0(CPU *cpu) { (void)cpu; unreachable(); }
void rt_call(CPU *cpu, uint32_t target) { (void)cpu; (void)target; unreachable(); }

void f_451A30(CPU *cpu)
{
    assert(M32(cpu->r[4]) == 0x8b3ef0 && M32(cpu->r[4] + 4) == ACTOR_FORM_ID);
    ++actor_lookups;
    cpu->r[0] = have_actor ? ACTOR : 0;
    cpu->r[4] += 4;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    uint32_t sp = cpu->r[4];
    switch (target) {
    case HOST_INVALID_HANDLE:
        assert(M32(sp) == 0x8b3b2f && cpu->r[1] == POLICY);
        cpu->r[0] = 0;
        cpu->r[2] = 0;
        cpu->r[4] += 4;
        break;
    case IS_TYPE_ADDRESS:
        f_8B3B20(cpu);
        break;
    case HOST_GET_EFFECT_LIST:
        assert(M32(sp) == 0x663418 && cpu->r[1] == ACTOR + 0x54);
        ++list_reads;
        cpu->r[0] = FIRST_NODE;
        cpu->r[4] += 4;
        break;
    case VISITOR_ADDRESS:
        assert(M32(sp) == 0x663446 && (M32(sp + 4) == DECOY || M32(sp + 4) == EFFECT));
        ++visits;
        if (observe) observed_effect_visitor(cpu);
        else f_8B3B00(cpu);
        break;
    default:
        unreachable();
    }
}

void highheels_observe_matching_effect(const CPU *cpu, uint32_t effect, uint16_t number)
{
    assert(cpu == invoker && observe && include_effect);
    assert(effect == EFFECT && number == EFFECT_NUMBER && M16(effect + 0x5c) == number);
    assert(actor_lookups == 1 && list_reads == 1 && visits == 2 && samples == 0);
    /* Read inside the matched visitor. The result stores only the Float bits. */
    magnitude = TL32(effect + 0x50);
    ++samples;
}

typedef struct Result {
    uint32_t registers[8];
    Flags flags;
    uint8_t stack[160];
} Result;

static Result run(uint32_t type, bool loaded_actor, uint8_t actor_type, bool present, bool probe)
{
    CPU cpu = {.r = {1, POLICY, 3, 4, STACK, 5, 6, 7}};
    memset(MEM + STACK - 144, 0, 160);
    M32(STACK) = 0x0123456;
    M32(STACK + 4) = EFFECT_NUMBER;
    M32(STACK + 8) = ACTOR_FORM_ID;
    f_8B39F0(&cpu);
    assert(cpu.r[0] == ACTOR_FORM_ID && cpu.r[2] == (0x20000 | EFFECT_NUMBER));
    M32(STACK + 4) = type;
    M32(STACK + 8) = cpu.r[0];
    M32(STACK + 12) = cpu.r[2];
    cpu = (CPU){.r = {1, POLICY, 3, 4, STACK, 5, 6, 7}};
    M8(ACTOR + 0x12) = actor_type;
    M32(FIRST_NODE + 4) = present ? SECOND_NODE : 0;
    uint8_t before[0x100];
    memcpy(before, MEM + DECOY, sizeof before);

    observe = probe;
    have_actor = loaded_actor;
    include_effect = present;
    invoker = &cpu;
    actor_lookups = list_reads = visits = samples = 0;
    magnitude = 0;
    f_8B3E30(&cpu);
    invoker = NULL;

    bool valid_type = type == ACTIVE_EFFECT_TYPE;
    bool actor_ok = valid_type && loaded_actor && actor_type == 0x3e;
    bool matched = actor_ok && present;
    assert(cpu.r[4] == STACK + 16 && cpu.r[0] == (matched ? EFFECT : 0));
    assert(actor_lookups == (unsigned)valid_type && list_reads == (unsigned)actor_ok);
    assert(visits == (actor_ok ? present ? 2u : 1u : 0u));
    assert(samples == (unsigned)(matched && probe));
    assert(magnitude == (matched && probe ? MAGNITUDE_BITS : 0));
    assert(!memcmp(before, MEM + DECOY, sizeof before));

    Result result = {.flags = cpu.flags};
    memcpy(result.registers, cpu.r, sizeof result.registers);
    memcpy(result.stack, MEM + STACK - 144, sizeof result.stack);
    return result;
}

int main(void)
{
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    const uint32_t locations[] = {STACK, FIND_EFFECT_TABLE};
    for (size_t i = 0; i < sizeof locations / sizeof locations[0]; ++i) {
        uint32_t address = locations[i];
        if (!gm_pointer(&space, address, 8))
            assert(gm_commit_page(&space, address / space.page_size * space.page_size, NULL));
    }
    MEM = space.base;
    M32(POLICY) = POLICY_TABLE;
    M32(POLICY_TABLE + 0x4) = IS_TYPE_ADDRESS;
    M32(POLICY_TABLE + 0xc) = HOST_INVALID_HANDLE;
    M32(ACTOR + 0x54) = TARGET_TABLE;
    M32(TARGET_TABLE + 0x1c) = HOST_GET_EFFECT_LIST;
    M32(FIRST_NODE) = DECOY;
    M32(SECOND_NODE) = EFFECT;
    M32(FIND_EFFECT_TABLE + 4) = VISITOR_ADDRESS;
    M16(DECOY + 0x5c) = EFFECT_NUMBER - 1;
    M32(DECOY + 0x50) = 0x3f800000;
    M16(EFFECT + 0x5c) = EFFECT_NUMBER;
    M32(EFFECT + 0x50) = MAGNITUDE_BITS;

    struct Scenario {
        uint32_t type;
        bool actor;
        uint8_t actor_type;
        bool effect;
    } scenarios[] = {
        {ACTIVE_EFFECT_TYPE, true, 0x3e, true},
        {ACTIVE_EFFECT_TYPE, true, 0x3e, false},
        {ACTIVE_EFFECT_TYPE, false, 0x3e, true},
        {ACTIVE_EFFECT_TYPE, true, 0x3d, true},
        {0x3d, true, 0x3e, true},
    };
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i) {
        const struct Scenario *s = &scenarios[i];
        Result plain = run(s->type, s->actor, s->actor_type, s->effect, false);
        Result captured = run(s->type, s->actor, s->actor_type, s->effect, true);
        assert(!memcmp(plain.registers, captured.registers, sizeof plain.registers));
        assert(!memcmp(&plain.flags, &captured.flags, sizeof plain.flags));
        assert(!memcmp(plain.stack, captured.stack, sizeof plain.stack));
    }
    assert(gm_release(&space, NULL));
    puts("Classic typed effect lookup captures magnitude only in the matching visitor");
    return 0;
}
