/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

enum {
    STACK = 0x2f00, ACTOR = 0x2300, CASTER = 0x2400, TARGET_SLOT = 0x2500, CASTER_SLOT = 0x2510,
    HANDLE = 0x2520, EVENT_SOURCE = 0x2600, SINK = EVENT_SOURCE + 0x60,
    LISTENERS = 0x2680, LISTENER = 0x2690, LISTENER_TABLE = 0x26a0,
    CAS_IMPORT = 0x106b14c, ADD_REF_IMPORT = 0x106b150, RELEASE_IMPORT = 0x106b154,
    REF_TABLE = 0x1310638, TARGET_INDEX = 3, CASTER_INDEX = 5, GENERATION = 0x200000,
    ACTOR_FORM_ID = 0x41234, EFFECT_NUMBER = 0xf223, ACTOR_TYPE = 0x3e,
    CAS = 1, ADD_REF, RELEASE, RECEIVE_EVENT
};

uint8_t *MEM;
static const CPU *invoking_owner;
static unsigned notifications, guard_releases, stale_releases;
static uint32_t observed_form_id;
static uint16_t observed_effect_number;
static uint8_t observed_phase;

static unsigned refs(uint32_t object)
{
    return M32(object + 0x18) & 0x3ff;
}

void f_401710(CPU *cpu)
{
    /* Authored uncontended dispatch guard. The guest performs the unlock. */
    assert(cpu == invoking_owner && cpu->r[1] == SINK);
    assert(M32(cpu->r[4]) == 0x6ebc20 && M32(cpu->r[4] + 4) == 0);
    assert(M32(SINK) == 0 && M32(SINK + 4) == 0);
    M32(SINK) = 1;
    M32(SINK + 4) = 1;
    cpu->r[4] += 8;
}

void f_A49B80(CPU *cpu) { (void)cpu; assert(0 && "unexpected listener addition"); }
void f_A4A070(CPU *cpu) { (void)cpu; assert(0 && "unexpected listener removal"); }
void rt_call(CPU *cpu, uint32_t target)
{
    (void)cpu;
    (void)target;
    assert(0 && "unexpected indirect cleanup");
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    uint32_t sp = cpu->r[4], return_address = M32(sp);
    switch (target) {
    case CAS:
        assert(cpu == invoking_owner && return_address == 0x6ebdfe);
        assert(M32(sp + 4) == SINK && M32(SINK + 4) == 0);
        assert(M32(SINK) == M32(sp + 12) && M32(SINK) == 1 && M32(sp + 8) == 0);
        cpu->r[0] = M32(SINK);
        M32(SINK) = 0;
        ++guard_releases;
        cpu->r[4] += 16;
        break;
    case ADD_REF:
    case RELEASE: {
        uint32_t address = M32(sp + 4);
        assert(address == ACTOR + 0x18 || address == CASTER + 0x18);
        if (target == ADD_REF) {
            assert(return_address == 0x4a81f1 || return_address == 0x656881 || return_address == 0x656895);
            cpu->r[0] = ++M32(address);
        } else {
            assert(return_address == 0x495273 || return_address == 0x44bb9b ||
                   return_address == 0x44bbbb || return_address == 0x40b124);
            stale_releases += return_address == 0x495273;
            cpu->r[0] = --M32(address);
            assert((cpu->r[0] & 0x3ff) >= 1);
        }
        cpu->r[4] += 8;
        break;
    }
    case RECEIVE_EVENT: {
        uint32_t event = M32(sp + 4);
        assert(cpu == invoking_owner && return_address == 0x6ebd4b);
        assert(cpu->r[1] == LISTENER && M32(sp + 8) == SINK);
        assert(M32(SINK) == 1 && M32(SINK + 4) == 1 && M8(SINK + 0x2c) == 1);
        uint32_t caster = M32(event), actor = M32(event + 4);
        assert(caster == M32(CASTER_SLOT) && actor == M32(TARGET_SLOT));
        if (caster)
            assert(refs(caster) == (actor == caster ? 5u : 3u));
        if (actor)
            assert(refs(actor) == (actor == caster ? 5u : 3u));
        /* The listener keeps values only. No guest pointer escapes dispatch. */
        if (actor && M8(actor + 0x12) == ACTOR_TYPE) {
            observed_form_id = M32(actor + 0xc);
            observed_effect_number = M16(event + 8);
            observed_phase = M8(event + 0xa);
        }
        ++notifications;
        cpu->r[0] = 0;
        cpu->r[4] += 12;
        break;
    }
    default:
        assert(0 && "unexpected imported or virtual call");
    }
}

static void resolve_slot(CPU *cpu, uint32_t slot, uint32_t handle)
{
    cpu->r[1] = slot;
    cpu->r[4] = STACK;
    M32(HANDLE) = handle;
    M32(STACK) = 0x100;
    M32(STACK + 4) = HANDLE;
    f_4952A0(cpu);
    assert(cpu->r[0] == slot && cpu->r[4] == STACK + 8);
}

static void release_slot(CPU *cpu, uint32_t slot)
{
    cpu->r[1] = slot;
    cpu->r[4] = STACK;
    M32(STACK) = 0x100;
    f_40B110(cpu);
    assert(cpu->r[4] == STACK + 4);
}

static void run(bool start, uint32_t target_handle, uint32_t caster_handle, uint8_t target_type)
{
    CPU cpu = {.r = {0, 0, 2, 3, STACK, 5, 6, 7}};
    invoking_owner = &cpu;
    notifications = guard_releases = stale_releases = 0;
    observed_form_id = observed_effect_number = 0;
    observed_phase = 0xff;
    M32(ACTOR + 0x18) = (TARGET_INDEX << 11) | 1;
    M32(CASTER + 0x18) = (CASTER_INDEX << 11) | 1;
    M8(ACTOR + 0x12) = target_type;

    resolve_slot(&cpu, TARGET_SLOT, target_handle);
    resolve_slot(&cpu, CASTER_SLOT, caster_handle);
    uint32_t actor = target_handle == (GENERATION | TARGET_INDEX) ? ACTOR : 0;
    uint32_t caster = caster_handle == (GENERATION | CASTER_INDEX) ? CASTER :
                      caster_handle == (GENERATION | TARGET_INDEX) ? ACTOR : 0;
    unsigned actor_slot_refs = 1u + (actor == ACTOR) + (caster == ACTOR);
    unsigned caster_slot_refs = 1u + (caster == CASTER);
    assert(M32(TARGET_SLOT) == actor && M32(CASTER_SLOT) == caster);
    assert(refs(ACTOR) == actor_slot_refs && refs(CASTER) == caster_slot_refs);

    cpu.r[1] = EVENT_SOURCE;
    cpu.r[4] = STACK;
    M32(STACK) = start ? 0x656ec5 : 0x65710b;
    M32(STACK + 4) = CASTER_SLOT;
    M32(STACK + 8) = TARGET_SLOT;
    M32(STACK + 12) = EFFECT_NUMBER;
    M32(STACK + 16) = start;
    f_656860(&cpu);

    assert(notifications == 1 && guard_releases == 1);
    assert(stale_releases == (unsigned)(target_handle != 0 && actor == 0));
    assert(cpu.r[4] == STACK + 20 && M32(SINK) == 0 && M8(SINK + 0x2c) == 0);
    assert(refs(ACTOR) == actor_slot_refs && refs(CASTER) == caster_slot_refs);
    if (actor && target_type == ACTOR_TYPE) {
        assert(observed_form_id == ACTOR_FORM_ID && observed_effect_number == EFFECT_NUMBER);
        assert(observed_phase == start);
    } else {
        assert(observed_form_id == 0 && observed_effect_number == 0 && observed_phase == 0xff);
    }

    release_slot(&cpu, CASTER_SLOT);
    release_slot(&cpu, TARGET_SLOT);
    assert(refs(ACTOR) == 1 && refs(CASTER) == 1);
    invoking_owner = NULL;
}

int main(void)
{
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    const uint32_t addresses[] = {STACK, CAS_IMPORT, REF_TABLE};
    for (unsigned i = 0; i < sizeof addresses / sizeof addresses[0]; ++i)
        assert(gm_commit_page(&space, addresses[i] / space.page_size * space.page_size, NULL));
    MEM = space.base;
    M32(CAS_IMPORT) = CAS;
    M32(ADD_REF_IMPORT) = ADD_REF;
    M32(RELEASE_IMPORT) = RELEASE;
    M32(REF_TABLE + 8 * TARGET_INDEX) = 0x4000000 | GENERATION;
    M32(REF_TABLE + 8 * TARGET_INDEX + 4) = ACTOR + 0x14;
    M32(REF_TABLE + 8 * CASTER_INDEX) = 0x4000000 | GENERATION;
    M32(REF_TABLE + 8 * CASTER_INDEX + 4) = CASTER + 0x14;
    M32(ACTOR + 0xc) = ACTOR_FORM_ID;
    M32(SINK + 8) = LISTENERS;
    M32(SINK + 0x10) = 1;
    M32(LISTENERS) = LISTENER;
    M32(LISTENER) = LISTENER_TABLE;
    M32(LISTENER_TABLE + 4) = RECEIVE_EVENT;

    const uint32_t target = GENERATION | TARGET_INDEX, caster = GENERATION | CASTER_INDEX;
    run(true, target, caster, ACTOR_TYPE);
    run(false, target, caster, ACTOR_TYPE);
    run(true, target, target, ACTOR_TYPE);
    run(false, target, 0, ACTOR_TYPE);
    run(false, 0, caster, ACTOR_TYPE);
    run(true, target ^ 0x100000, caster, ACTOR_TYPE);
    run(true, target, caster, 0x3d);
    assert(gm_release(&space, NULL));
    puts("Classic effect events lend the target actor through synchronous native dispatch");
    return 0;
}
