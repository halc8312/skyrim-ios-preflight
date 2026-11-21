/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STACK = 0x2f00, ACTOR = 0x2300, CASTER = 0x2400, TARGET_SLOT = 0x2500, CASTER_SLOT = 0x2510,
    HANDLE = 0x2520, EVENT_SOURCE = 0x12e4c30, SINK = EVENT_SOURCE + 0x60,
    LISTENERS = 0x2680, LISTENER = 0x2690, LISTENER_TABLE = 0x26a0,
    EFFECT = 0x2a00, ITEM = 0x2a80, SETTING = 0x2aa0, TARGET = ACTOR + 0x54, TARGET_TABLE = 0x2700,
    ACTOR_TABLE = 0x2b00,
    SCRIPT_EFFECT_TABLE = 0x10c9de4, EVENT_INIT = 0x12e5620,
    CAS_IMPORT = 0x106b14c, ADD_REF_IMPORT = 0x106b150, RELEASE_IMPORT = 0x106b154,
    REF_TABLE = 0x1310638, TARGET_INDEX = 3, CASTER_INDEX = 5, GENERATION = 0x200000,
    ACTOR_FORM_ID = 0x41234, EFFECT_NUMBER = 0xf223, ACTOR_TYPE = 0x3e, MGEF_TYPE = 0x12,
    PRODUCER_STACK_BELOW = 200,
    HIGH_HEEL_FORM_ID = 0x42000800, BASE_MAGNITUDE = 0x41000000,
    CAS = 1, ADD_REF, RELEASE, RECEIVE_EVENT, TARGET_REF, TARGET_IS_ACTOR, ACTOR_CHECK
};

uint8_t *MEM;
static const CPU *invoking_owner;
static unsigned notifications, guard_releases, stale_releases;
static uint32_t observed_form_id;
static uint16_t observed_effect_number;
static uint8_t observed_phase;
static bool producer_mode;
static uint32_t producer_handle, producer_target;
static uint32_t call_sites[32];
static size_t call_count;
typedef struct {
    uint32_t form_id, setting_id, number, magnitude, phase;
} PhaseEvidence;
static PhaseEvidence producer_evidence;
static unsigned producer_attempts;

void observed_effect_producer(CPU *);

static unsigned refs(uint32_t object)
{
    return M32(object + 0x18) & 0x3ff;
}

void highheels_observe_phase(const CPU *cpu, uint32_t effect, uint32_t source, uint32_t args, uint32_t site)
{
    assert(cpu == invoking_owner && producer_mode && effect == EFFECT);
    assert(source == EVENT_SOURCE && cpu->r[0] == source);
    assert(cpu->r[4] == args && cpu->r[6] == effect);
    uint32_t phase = M32(args + 12);
    assert((phase == 1 && site == 0x656ec5) || (phase == 0 && site == 0x65710b));
    assert(M32(args + 8) == EFFECT_NUMBER && M16(effect + 0x5c) == EFFECT_NUMBER);
    assert(M32(M32(args)) == 0);
    uint32_t actor = M32(M32(args + 4));
    assert(actor == producer_target && (!actor || refs(actor) == 2));
    ++producer_attempts;
    if (!actor || M8(actor + 0x12) != ACTOR_TYPE) return;
    uint32_t item = M32(effect + 0x34);
    if (!item) return;
    uint32_t setting = M32(item + 0xc);
    if (!setting || M8(setting + 0x12) != MGEF_TYPE) return;
    uint32_t id = M32(setting + 0xc), magnitude = M32(effect + 0x50);
    if (id != HIGH_HEEL_FORM_ID || (magnitude & 0x7f800000u) == 0x7f800000u) return;
    assert(M32(item) == BASE_MAGNITUDE && magnitude != BASE_MAGNITUDE);
    producer_evidence = (PhaseEvidence){M32(actor + 0xc), id, EFFECT_NUMBER, magnitude, phase};
}

static void trace_call(uint32_t site)
{
    if (producer_mode) {
        assert(call_count < sizeof call_sites / sizeof call_sites[0]);
        call_sites[call_count++] = site;
    }
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
/* Used by original handle manufacture. The authored handle can be stale. */
void f_65CC00(CPU *cpu)
{
    uint32_t sp = cpu->r[4], site = M32(sp);
    assert(cpu == invoking_owner && producer_mode && site == 0x6bd6e8);
    assert(M32(sp + 8) == ACTOR);
    M32(M32(sp + 4)) = producer_handle;
    trace_call(site);
    cpu->r[4] += 4;
}

/* Author a value returned by the effect number allocator. */
void f_656C50(CPU *cpu)
{
    assert(cpu->r[1] == EFFECT && M32(cpu->r[4]) == 0x656e7e);
    trace_call(M32(cpu->r[4]));
    cpu->r[0] = EFFECT_NUMBER;
    cpu->r[4] += 4;
}

void f_4D52C0(CPU *cpu)
{
    uint32_t site = M32(cpu->r[4]);
    assert(cpu->r[1] == ACTOR && (site == 0x656db1 || site == 0x656f9a));
    trace_call(site);
    cpu->r[0] = site == 0x656f9a;
    cpu->r[4] += 4;
}

/* The owner MGEF has no sound entries. */
void f_655080(CPU *cpu)
{
    uint32_t site = M32(cpu->r[4]);
    assert(cpu->r[1] == EFFECT && (site == 0x656dc6 || site == 0x656fc0));
    trace_call(site);
    cpu->r[4] += 4;
}

/* The authored object has no controller to detach. */
void f_B9A1F0(CPU *cpu)
{
    assert(cpu->r[1] == EFFECT + 0x1c && M32(cpu->r[4]) == 0x6570b7);
    trace_call(M32(cpu->r[4]));
    cpu->r[0] = 0;
    cpu->r[4] += 4;
}

#define UNEXPECTED_BODY(address) \
    void f_##address(CPU *cpu) { (void)cpu; assert(0 && "unexpected guest path " #address); }
UNEXPECTED_BODY(406A60)
UNEXPECTED_BODY(406BE0)
UNEXPECTED_BODY(436B90)
UNEXPECTED_BODY(48CB30)
UNEXPECTED_BODY(513E70)
UNEXPECTED_BODY(513EA0)
UNEXPECTED_BODY(513ED0)
UNEXPECTED_BODY(515E00)
UNEXPECTED_BODY(516020)
UNEXPECTED_BODY(6557C0)
UNEXPECTED_BODY(6560E0)
UNEXPECTED_BODY(6EA0E0)
UNEXPECTED_BODY(A487B0)
UNEXPECTED_BODY(B99F40)
UNEXPECTED_BODY(F520BC)
#undef UNEXPECTED_BODY

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
    trace_call(return_address);
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
        if (producer_mode) {
            assert(!caster && actor == producer_target);
            assert(M16(event + 8) == EFFECT_NUMBER);
        } else {
            assert(caster == M32(CASTER_SLOT) && actor == M32(TARGET_SLOT));
        }
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
    case TARGET_REF:
    case TARGET_IS_ACTOR:
        assert(cpu->r[1] == TARGET);
        assert(return_address == (target == TARGET_REF ? 0x656d5f : 0x66287a));
        cpu->r[0] = target == TARGET_REF ? ACTOR : 1;
        cpu->r[4] += 4;
        break;
    case ACTOR_CHECK:
        assert(cpu->r[1] == ACTOR && return_address == 0x656ffb && M32(sp + 4) == 1);
        cpu->r[0] = 0;
        cpu->r[4] += 8;
        break;
    case 0x6691f0:
        assert(cpu->r[1] == EFFECT && return_address == 0x656df4);
        f_6691F0(cpu);
        break;
    case 0x669220:
        assert(cpu->r[1] == EFFECT && return_address == 0x656fd1);
        f_669220(cpu);
        break;
    case 0x669340:
        assert(cpu->r[1] == EFFECT && return_address == 0x65709f);
        f_669340(cpu);
        break;
    case 0x654e60:
        assert(cpu->r[1] == EFFECT && return_address == 0x65705b);
        f_654E60(cpu);
        break;
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

typedef struct {
    CPU cpu;
    uint32_t call_sites[32];
    size_t call_count;
    uint32_t form_id, number, phase;
    uint8_t stack[PRODUCER_STACK_BELOW + 12], effect[0x6c];
} ProducerResult;

static void run_producer(bool observed, bool start, uint32_t setting, uint32_t magnitude,
                         uint32_t handle, uint8_t target_type, uint8_t setting_type, ProducerResult *result)
{
    producer_mode = true;
    producer_handle = handle;
    producer_target = handle == (GENERATION | TARGET_INDEX) ? ACTOR : 0;
    producer_attempts = 0;
    producer_evidence = (PhaseEvidence){0};
    call_count = 0;
    memset(call_sites, 0, sizeof call_sites);
    notifications = guard_releases = stale_releases = 0;
    observed_form_id = observed_effect_number = 0;
    observed_phase = 0xff;
    M32(ACTOR + 0x18) = (TARGET_INDEX << 11) | 1;
    M8(ACTOR + 0x12) = target_type;
    memset(MEM + EFFECT, 0, sizeof result->effect);
    M32(EFFECT) = SCRIPT_EFFECT_TABLE;
    M32(EFFECT + 0x1c) = UINT32_MAX;
    M32(EFFECT + 0x34) = ITEM;
    M32(EFFECT + 0x38) = handle ? TARGET : 0;
    M32(EFFECT + 0x50) = magnitude;
    M32(EFFECT + 0x54) = start ? 0x80000100u : 0x80050100u;
    M16(EFFECT + 0x5c) = start ? 0 : EFFECT_NUMBER;
    M32(ITEM) = BASE_MAGNITUDE;
    M32(ITEM + 0xc) = SETTING;
    M32(SETTING + 8) = 0x00400000u; /* Has attached VM data after loading. */
    M32(SETTING + 0xc) = setting;
    M8(SETTING + 0x12) = setting_type;
    M32(SETTING + 0x38) = 0x08200210u; /* Exact owner MGEF DATA flags. */
    memset(MEM + STACK - PRODUCER_STACK_BELOW, 0x78, sizeof result->stack);
    M32(STACK) = 0x100u;
    M32(STACK + 4) = 0; /* Native update time. */

    CPU cpu = {0};
    cpu.r[1] = EFFECT;
    cpu.r[4] = STACK;
    cpu.r[3] = 0xaaaau;
    cpu.r[5] = 0xbbbbu;
    cpu.r[6] = 0xccccu;
    cpu.r[7] = 0xddddu;
    invoking_owner = &cpu;
    if (observed) observed_effect_producer(&cpu);
    else f_656CB0(&cpu);
    invoking_owner = NULL;
    producer_mode = false;
    assert(cpu.r[4] == STACK + 8 && M32(STACK) == 0x100u);
    assert(cpu.r[3] == 0xaaaau && cpu.r[5] == 0xbbbbu && cpu.r[6] == 0xccccu && cpu.r[7] == 0xddddu);
    assert(notifications == (unsigned)(handle != 0) && guard_releases == notifications);
    assert(stale_releases == (unsigned)(handle != 0 && producer_target == 0));
    assert(refs(ACTOR) == 1 && M32(SINK) == 0 && M8(SINK + 0x2c) == 0);
    assert(M32(EFFECT + 0x34) == ITEM && M32(EFFECT + 0x50) == magnitude);
    if (producer_target && target_type == ACTOR_TYPE) {
        assert(observed_form_id == ACTOR_FORM_ID && observed_effect_number == EFFECT_NUMBER);
        assert(observed_phase == start);
    } else {
        assert(observed_form_id == 0 && observed_effect_number == 0 && observed_phase == 0xff);
    }

    result->cpu = cpu;
    memcpy(result->call_sites, call_sites, sizeof call_sites);
    result->call_count = call_count;
    result->form_id = observed_form_id;
    result->number = observed_effect_number;
    result->phase = observed_phase;
    memcpy(result->stack, MEM + STACK - PRODUCER_STACK_BELOW, sizeof result->stack);
    memcpy(result->effect, MEM + EFFECT, sizeof result->effect);
}

static void compare_producers(bool start, uint32_t setting, uint32_t magnitude,
                              uint32_t handle, uint8_t target_type, uint8_t setting_type, bool accept)
{
    ProducerResult original = {0}, observed = {0};
    run_producer(false, start, setting, magnitude, handle, target_type, setting_type, &original);
    assert(producer_attempts == 0);
    run_producer(true, start, setting, magnitude, handle, target_type, setting_type, &observed);
    assert(producer_attempts == (unsigned)(handle != 0));
    assert(!memcmp(original.cpu.r, observed.cpu.r, sizeof original.cpu.r));
    assert(!memcmp(&original.cpu.flags, &observed.cpu.flags, sizeof original.cpu.flags));
    assert(original.cpu.top == observed.cpu.top &&
           !memcmp(original.cpu.st, observed.cpu.st, sizeof original.cpu.st));
    assert(!memcmp(original.stack, observed.stack, sizeof original.stack));
    assert(!memcmp(original.effect, observed.effect, sizeof original.effect));
    assert(original.call_count == observed.call_count &&
           !memcmp(original.call_sites, observed.call_sites, sizeof original.call_sites));
    assert(original.form_id == observed.form_id && original.number == observed.number &&
           original.phase == observed.phase);
    if (accept) {
        assert(producer_evidence.form_id == ACTOR_FORM_ID);
        assert(producer_evidence.setting_id == HIGH_HEEL_FORM_ID);
        assert(producer_evidence.number == EFFECT_NUMBER && producer_evidence.phase == start);
        assert(producer_evidence.magnitude == magnitude);
    } else {
        assert(!producer_evidence.form_id && !producer_evidence.setting_id && !producer_evidence.number);
    }
}

int main(void)
{
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    const uint32_t addresses[] = {STACK, CAS_IMPORT, REF_TABLE, EVENT_SOURCE, EVENT_INIT, SCRIPT_EFFECT_TABLE};
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

    M8(EVENT_INIT) = 1;
    M32(TARGET) = TARGET_TABLE;
    M32(TARGET_TABLE + 8) = TARGET_REF;
    M32(TARGET_TABLE + 0xc) = TARGET_IS_ACTOR;
    M32(ACTOR) = ACTOR_TABLE;
    M32(ACTOR_TABLE + 0x264) = ACTOR_CHECK;
    M32(SCRIPT_EFFECT_TABLE + 0x10) = 0x669220;
    M32(SCRIPT_EFFECT_TABLE + 0x50) = 0x6691f0;
    M32(SCRIPT_EFFECT_TABLE + 0x54) = 0x669340;
    M32(SCRIPT_EFFECT_TABLE + 0x58) = 0x654e60;
    compare_producers(true, HIGH_HEEL_FORM_ID, 0x413ccccdu, target, ACTOR_TYPE, MGEF_TYPE, true);
    compare_producers(true, HIGH_HEEL_FORM_ID, 0x40e80000u, target, ACTOR_TYPE, MGEF_TYPE, true);
    compare_producers(false, HIGH_HEEL_FORM_ID, 0x41700000u, target, ACTOR_TYPE, MGEF_TYPE, true);
    compare_producers(true, HIGH_HEEL_FORM_ID ^ 0x1000000u, 0x413ccccdu, target, ACTOR_TYPE, MGEF_TYPE, false);
    compare_producers(true, HIGH_HEEL_FORM_ID + 1, 0x413ccccdu, target, ACTOR_TYPE, MGEF_TYPE, false);
    compare_producers(true, HIGH_HEEL_FORM_ID, 0x413ccccdu, target ^ 0x100000u, ACTOR_TYPE, MGEF_TYPE, false);
    compare_producers(true, HIGH_HEEL_FORM_ID, 0x413ccccdu, 0, ACTOR_TYPE, MGEF_TYPE, false);
    compare_producers(false, HIGH_HEEL_FORM_ID, 0x41700000u, target, 0x3d, MGEF_TYPE, false);
    compare_producers(true, HIGH_HEEL_FORM_ID, 0x413ccccdu, target, ACTOR_TYPE, 0x16, false);
    compare_producers(false, HIGH_HEEL_FORM_ID, 0x7fc00000u, target, ACTOR_TYPE, MGEF_TYPE, false);
    assert(gm_release(&space, NULL));
    puts("Classic effect events lend the target and preserve effect identity and magnitude through native dispatch");
    return 0;
}
