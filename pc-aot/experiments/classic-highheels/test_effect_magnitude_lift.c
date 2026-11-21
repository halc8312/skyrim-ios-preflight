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
    DECOY = 0x2400, EFFECT = 0x2480, VALUE = 0x2500, IDENTIFIER = 0x2520,
    ACTOR_PROCESS = 0x2600, PROCESS_DATA = 0x2700, EFFECT_CLASS = 0x2a00,
    OTHER_ACTOR = 0x2b00,
    TARGET_CALL_STACK = 0x2c00,
    ACTIVE_EFFECT_TYPE = 0x8b, ACTOR_FORM_ID = 0x17345, EFFECT_NUMBER = 0x42,
    FIND_EFFECT_TABLE = 0x10ea224, VISITOR_ADDRESS = 0x8b3b00, IS_TYPE_ADDRESS = 0x8b3b20,
    POLICY_GLOBAL = 0x1ba7008, VALUE_INITIALIZED = 0x1ba7024, SLEEP_IMPORT = 0x106b148,
    CAS_IMPORT = 0x106b14c, INVALID_HANDLE = 0x8b4300, IS_VALID = 0x8b3d60, RESOLVE = 0x8b3e30,
    GET_EFFECT_LIST = 0x6e9180, HOST_SLEEP = 2, HOST_CAS = 3, HOST_IS_ACTOR_TARGET = 4,
    MAGNITUDE_BITS = 0x40b00000
};

uint8_t *MEM;
static bool observe, have_actor, include_effect;
static unsigned actor_lookups, list_reads, visits, samples;
static unsigned validity_lookups, cas_calls, sleeps;
static uint32_t magnitude;
static const CPU *invoker;
static bool verify_owner, keep_call, admitted;
static uint32_t supplied_actor, supplied_magnitude;
static bool observe_identifier, replace_before_second_snapshot;
static uint64_t handle_snapshots[2];
static unsigned snapshot_count;

void f_6E9180(CPU *);
void f_8B39F0(CPU *);
void f_8B3B00(CPU *);
void f_8B3B20(CPU *);
void f_8B3D60(CPU *);
void f_8B3E30(CPU *);
void f_8B4300(CPU *);
void f_8D8500(CPU *);
void f_8D8F10(CPU *);
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
void f_F520BC(CPU *cpu) { (void)cpu; unreachable(); }
void rt_call(CPU *cpu, uint32_t target) { (void)cpu; (void)target; unreachable(); }

void f_451A30(CPU *cpu)
{
    uint32_t ret = M32(cpu->r[4]);
    assert(M32(cpu->r[4] + 4) == ACTOR_FORM_ID);
    if (ret == 0x8b3e00)
        ++validity_lookups;
    else {
        assert(ret == 0x8b3ef0);
        ++actor_lookups;
    }
    cpu->r[0] = have_actor ? ACTOR : 0;
    cpu->r[4] += 4;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    uint32_t sp = cpu->r[4];
    switch (target) {
    case INVALID_HANDLE:
        assert(cpu->r[1] == POLICY);
        f_8B4300(cpu);
        break;
    case IS_TYPE_ADDRESS:
        f_8B3B20(cpu);
        if (M32(sp) == 0x8d86af && replace_before_second_snapshot)
            M32(IDENTIFIER + 0x14) = 0x20000 | EFFECT_NUMBER;
        break;
    case IS_VALID:
        f_8B3D60(cpu);
        break;
    case RESOLVE:
        f_8B3E30(cpu);
        break;
    case HOST_CAS: {
        assert(M32(sp) == 0xc3080e && M32(sp + 4) == IDENTIFIER + 0x18);
        uint32_t previous = M32(IDENTIFIER + 0x18);
        uint32_t expected = M32(sp + 12);
        assert(M32(sp + 8) == (expected | UINT32_C(0x80000000)));
        if (previous == expected)
            M32(IDENTIFIER + 0x18) = M32(sp + 8);
        cpu->r[0] = previous;
        cpu->r[4] += 16;
        ++cas_calls;
        break;
    }
    case HOST_SLEEP:
        assert(M32(sp) == 0xc307f6 && M32(sp + 4) == 0);
        assert(++sleeps == 1 && M32(IDENTIFIER + 0x18) == UINT32_C(0x80000002));
        M32(IDENTIFIER + 0x18) = 2; /* Authored other owner releases the lock. */
        cpu->r[4] += 8;
        break;
    case HOST_IS_ACTOR_TARGET:
        assert(M32(sp) == 0x66287a && cpu->r[1] == ACTOR + 0x54);
        cpu->r[0] = 1;
        cpu->r[4] += 4;
        break;
    case GET_EFFECT_LIST:
        assert(M32(sp) == 0x663418 && cpu->r[1] == ACTOR + 0x54);
        ++list_reads;
        f_6E9180(cpu);
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

void highheels_observe_identifier(const CPU *cpu, uint32_t site, uint32_t low, uint32_t high)
{
    if (!observe_identifier)
        return;
    assert(cpu == invoker && snapshot_count < 2);
    assert(site == (snapshot_count ? 0x8d8f47u : 0x8d869du));
    assert(M32(IDENTIFIER + 0x18) == UINT32_C(0x80000002));
    handle_snapshots[snapshot_count++] = (uint64_t)high << 32 | low;
}

void highheels_observe_matching_effect(const CPU *cpu, uint32_t effect, uint16_t number)
{
    assert(cpu == invoker && observe && include_effect);
    assert(effect == EFFECT && number == EFFECT_NUMBER && M16(effect + 0x5c) == number);
    assert(actor_lookups == 1 && list_reads == 1 && visits == 2 && samples == 0);
    /* Read inside the matched visitor. No effect address is returned by the observer. */
    magnitude = TL32(effect + 0x50);
    if (verify_owner) {
        /* Run the stock GetTargetActor body on fixture-owned scratch. */
        CPU target_cpu = *cpu;
        target_cpu.r[4] = TARGET_CALL_STACK;
        M32(TARGET_CALL_STACK + 12) = effect;
        f_8D8500(&target_cpu);
        uint32_t target = target_cpu.r[0];
        bool consistent = !observe_identifier ||
                          (snapshot_count == 2 && handle_snapshots[0] == handle_snapshots[1]);
        admitted = target && target == supplied_actor &&
                   (!keep_call || magnitude == supplied_magnitude) && consistent;
    }
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

typedef struct OwnerScenario {
    const char *description;
    uint32_t type, identifier, handle_hi, effect_target, actor, magnitude;
    bool actor_missing, actor_deleted, effect_missing, load_process_missing;
    bool lock_busy, keep, found, admitted, replace_between_reads;
} OwnerScenario;

static void check_owner(const OwnerScenario *s, bool probe)
{
    assert(s->description);
    CPU cpu = {.r = {1, 2, 3, 4, STACK, 5, 6, 7}};
    M32(STACK + 4) = VALUE;
    M32(STACK + 8) = 0;
    M32(VALUE) = s->type;
    M32(VALUE + 4) = s->identifier;
    uint32_t handle_low = s->handle_hi == 0xffff ? 0 : ACTOR_FORM_ID;
    M32(IDENTIFIER + 0x10) = handle_low;
    M32(IDENTIFIER + 0x14) = s->handle_hi;
    M32(IDENTIFIER + 0x18) = s->lock_busy ? UINT32_C(0x80000002) : 2;
    M32(EFFECT + 0x38) = s->effect_target;
    M8(ACTOR + 0x12) = 0x3e;
    M32(ACTOR + 0x8) = s->actor_deleted ? UINT32_C(1) << 21 : 0;
    M32(ACTOR + 0x88) = s->load_process_missing ? 0 : ACTOR_PROCESS;
    M32(FIRST_NODE + 4) = s->effect_missing ? 0 : SECOND_NODE;
    observe = verify_owner = true;
    observe_identifier = probe;
    replace_before_second_snapshot = s->replace_between_reads;
    snapshot_count = 0;
    have_actor = !s->actor_missing;
    include_effect = !s->effect_missing;
    keep_call = s->keep;
    supplied_actor = s->actor;
    supplied_magnitude = s->magnitude;
    admitted = false;
    cas_calls = sleeps = validity_lookups = 0;
    actor_lookups = list_reads = visits = samples = 0;
    invoker = &cpu;
    f_8D8F10(&cpu);
    invoker = NULL;
    bool expected_admitted = s->admitted && (!probe || !s->replace_between_reads);
    if (samples != (unsigned)s->found || admitted != expected_admitted)
        fprintf(stderr, "unexpected owner result: %s\n", s->description);
    assert(cpu.r[4] == STACK + 4 && cpu.r[0] == (s->found ? EFFECT : 0));
    assert(samples == (unsigned)s->found && admitted == expected_admitted);
    unsigned expected_cas = 0;
    if (s->identifier && s->type == EFFECT_CLASS)
        expected_cas = s->handle_hi == 0xffff ? 1u : 2u;
    assert(cas_calls == expected_cas);
    assert(snapshot_count == (probe ? expected_cas : 0));
    assert(validity_lookups == (unsigned)(expected_cas == 2));
    assert(sleeps == (unsigned)s->lock_busy && M32(IDENTIFIER + 0x18) == 2);
    assert(M32(VALUE) == s->type && M32(VALUE + 4) == s->identifier);
    uint32_t final_handle_hi = s->replace_between_reads ? (0x20000 | EFFECT_NUMBER) : s->handle_hi;
    assert(M32(IDENTIFIER + 0x10) == handle_low && M32(IDENTIFIER + 0x14) == final_handle_hi);
    if (snapshot_count)
        assert(handle_snapshots[0] == ((uint64_t)s->handle_hi << 32 | handle_low));
    if (snapshot_count == 2)
        assert(handle_snapshots[1] == ((uint64_t)final_handle_hi << 32 | handle_low));
    assert(M32(EFFECT + 0x38) == s->effect_target);
}

int main(void)
{
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    const uint32_t locations[] = {STACK, FIND_EFFECT_TABLE, POLICY_GLOBAL, CAS_IMPORT};
    for (size_t i = 0; i < sizeof locations / sizeof locations[0]; ++i) {
        uint32_t address = locations[i];
        if (!gm_pointer(&space, address, 8))
            assert(gm_commit_page(&space, address / space.page_size * space.page_size, NULL));
    }
    MEM = space.base;
    M32(POLICY) = POLICY_TABLE;
    M32(POLICY_TABLE + 0x4) = IS_TYPE_ADDRESS;
    M32(POLICY_TABLE + 0x8) = IS_VALID;
    M32(POLICY_TABLE + 0xc) = INVALID_HANDLE;
    M32(POLICY_TABLE + 0x20) = RESOLVE;
    M32(POLICY_GLOBAL) = POLICY;
    M32(VALUE_INITIALIZED) = 1;
    M32(CAS_IMPORT) = HOST_CAS;
    M32(SLEEP_IMPORT) = HOST_SLEEP;
    M32(ACTOR + 0x54) = TARGET_TABLE;
    M32(TARGET_TABLE + 0x1c) = GET_EFFECT_LIST;
    M32(TARGET_TABLE + 0xc) = HOST_IS_ACTOR_TARGET;
    M32(ACTOR + 0x88) = ACTOR_PROCESS;
    M32(ACTOR_PROCESS + 0x4) = PROCESS_DATA;
    M32(PROCESS_DATA + 0xf8) = FIRST_NODE;
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
    const OwnerScenario live = {
        .description = "live effect with actor", .type = EFFECT_CLASS, .identifier = IDENTIFIER,
        .handle_hi = 0x20000 | EFFECT_NUMBER, .effect_target = ACTOR + 0x54, .actor = ACTOR,
        .magnitude = MAGNITUDE_BITS, .found = true, .admitted = true,
    };
    check_owner(&live, false);
    check_owner(&live, true);
    OwnerScenario keep = live;
    keep.description = "live keep while the identifier lock is contended once";
    keep.keep = keep.lock_busy = true;
    check_owner(&keep, true);
    keep.description = "wrong actor";
    keep.lock_busy = keep.admitted = false;
    keep.actor = OTHER_ACTOR;
    check_owner(&keep, true);
    keep.description = "changed magnitude";
    keep.actor = ACTOR;
    keep.magnitude++;
    check_owner(&keep, true);
    keep = live;
    keep.description = "changed identifier between the two stock snapshots";
    keep.keep = keep.replace_between_reads = true;
    keep.handle_hi++;
    check_owner(&keep, false);
    check_owner(&keep, true);
    OwnerScenario stale = live;
    stale.description = "no associated magic target";
    stale.effect_target = 0;
    stale.admitted = false;
    check_owner(&stale, true);
    stale.description = "old effect number on a still-loaded actor";
    stale.effect_target = live.effect_target;
    stale.handle_hi++;
    stale.found = false;
    check_owner(&stale, true);
    stale.description = "actor process absent";
    stale.handle_hi = live.handle_hi;
    stale.load_process_missing = true;
    check_owner(&stale, true);
    stale.description = "effect removed";
    stale.load_process_missing = false;
    stale.effect_missing = true;
    check_owner(&stale, true);
    stale.description = "actor no longer loaded";
    stale.actor_missing = true;
    check_owner(&stale, true);
    stale.description = "deleted actor rejected by the handle policy";
    stale.actor_missing = stale.effect_missing = false;
    stale.actor_deleted = true;
    check_owner(&stale, true);
    stale = live;
    stale.description = "engine invalid handle";
    stale.handle_hi = 0xffff;
    stale.found = stale.admitted = false;
    check_owner(&stale, true);
    stale.description = "array of script instances";
    stale.handle_hi = live.handle_hi;
    stale.type |= 1u;
    check_owner(&stale, true);
    stale.description = "null identifier";
    stale.type = EFFECT_CLASS;
    stale.identifier = 0;
    check_owner(&stale, true);
    assert(gm_release(&space, NULL));
    puts("Classic typed effect lookup captures magnitude only in the matching visitor");
    return 0;
}
