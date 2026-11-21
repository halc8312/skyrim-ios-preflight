/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    TLS_INDEX = 0x1bbeb54, TLS = 0x1000, TLS_TABLE = 0x1100, TLS_DATA = 0x1200,
    STACK_REF = 0x1800, STACK = 0x1900, OTHER_STACK = 0x1a00,
    FRAME = 0x1b00, CALLER = 0x1c00, OUTER = 0x1d00, SP = 0x3f00,
    KEEP = 0x4000, STOP = 0x4100, NATIVE_TABLE = 0x4200, REGISTRY = 0x4300,
    MOVE_UP = 0x4400, ON_UPDATE = 0x4500, MOVE_DOWN = 0x4600,
    EFFECT_CLASS = 0x4700, FIRST_SELF = 0x4800, SECOND_SELF = 0x4900,
    SLOT_CHECK = 1, SLOT_RUN = 2, RUN_RETURN = 0xc46e61, STACK_ID = 73, EPOCH = 7,
};

typedef struct AuthoredBindings {
    uint32_t registry, epoch, keep, stop, move_up, on_update, move_down, effect_class;
} AuthoredBindings;

uint8_t *MEM;
static gm_space space;
static const AuthoredBindings bindings = {
    REGISTRY, EPOCH, KEEP, STOP, MOVE_UP, ON_UPDATE, MOVE_DOWN, EFFECT_CLASS,
};
static bool observe;
static unsigned runs, observations;
static uint32_t expected_self;

void f_C46CB0(CPU *);

/* Model authenticated function/type identities. Read only while the owner
 * is inside this ordinary Run callback. The observer borrows the VMValue. */
static bool observe_caller(const CPU *cpu, const AuthoredBindings *auth,
                           uint32_t epoch, void (*read_self)(const uint8_t *))
{
    uint32_t sp = cpu->r[4];
    if (epoch != auth->epoch || !gm_pointer(&space, sp, 24) ||
        M32(sp) != RUN_RETURN || M32(sp + 8) != auth->registry)
        return false;
    uint32_t frame = M32(sp + 20);
    if (!frame || !gm_pointer(&space, frame, 12) || M32(frame + 8) != cpu->r[1])
        return false;
    uint32_t stack = M32(frame);
    if (!stack || !gm_pointer(&space, stack, 0x48) || M32(stack + 0x30) != frame ||
        M32(stack + 0x44) != M32(sp + 12))
        return false;
    uint32_t caller = M32(frame + 4);
    if (!caller || caller == frame || !gm_pointer(&space, caller, 0x1c) || M32(caller) != stack)
        return false;
    uint32_t function = M32(caller + 8);
    bool keep = cpu->r[1] == auth->keep &&
                (function == auth->move_up || function == auth->on_update);
    bool stop = cpu->r[1] == auth->stop && function == auth->move_down;
    if ((!keep && !stop) || M32(caller + 0x14) != auth->effect_class ||
        !M32(caller + 0x18))
        return false;
    read_self(MEM + caller + 0x14);
    return true;
}

static void check_borrowed_self(const uint8_t *value)
{
    assert(value == MEM + CALLER + 0x14);
    uint32_t payload;
    memcpy(&payload, value + 4, sizeof payload);
    assert(payload == expected_self);
    ++observations;
}

void f_C329E0(CPU *cpu)
{
    const uint32_t sp = cpu->r[4], destination = cpu->r[1], source = M32(sp + 4);
    if (M32(sp) == 0xc46d2e)
        assert(source == FRAME + 0x14 && M32(source) == 0);
    else
        assert(M32(sp) == 0xc46e7d && destination == STACK + 0x3c);
    memcpy(MEM + destination, MEM + source, 8);
    cpu->r[4] += 8;
}

void f_C328E0(CPU *cpu)
{
    assert(M32(cpu->r[4]) == 0xc46e1c || M32(cpu->r[4]) == 0xc46e25);
    assert(M32(cpu->r[1]) == 0);
    cpu->r[4] += 4;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    const uint32_t sp = cpu->r[4];
    if (target == SLOT_CHECK) {
        assert(M32(sp) == 0xc46d37);
        cpu->r[0] = 1;
        cpu->r[4] += 4;
        return;
    }
    assert(target == SLOT_RUN && M32(sp) == RUN_RETURN);
    assert(M32(sp + 8) == REGISTRY && M32(sp + 12) == STACK_ID && M32(sp + 20) == FRAME);
    assert(M32(M32(sp + 4)) == 0);
    ++runs;
    if (observe) {
        assert(!observe_caller(cpu, &bindings, EPOCH + 1, check_borrowed_self));
        AuthoredBindings other = bindings;
        other.registry += 4;
        assert(!observe_caller(cpu, &other, EPOCH, check_borrowed_self));
        /* Model another invocation running on this stack. Even while the
         * outer frame is allocated, its callback is no longer current. */
        M32(STACK + 0x30) = OUTER;
        assert(!observe_caller(cpu, &bindings, EPOCH, check_borrowed_self));
        M32(STACK + 0x30) = FRAME;
        M32(STACK + 0x44) = STACK_ID + 1;
        assert(!observe_caller(cpu, &bindings, EPOCH, check_borrowed_self));
        M32(STACK + 0x44) = STACK_ID;
        assert(observe_caller(cpu, &bindings, EPOCH, check_borrowed_self) == !!expected_self);
    }
    cpu->r[0] = 1;
    cpu->r[4] += 24;
}

#define UNUSED_BRANCH(name) \
    void name(CPU *cpu) { (void)cpu; assert(0 && "unexpected native invoke branch"); }
UNUSED_BRANCH(f_C4D650)
UNUSED_BRANCH(f_505D00)
UNUSED_BRANCH(f_4202A0)
UNUSED_BRANCH(f_C395D0)
UNUSED_BRANCH(f_C398A0)
UNUSED_BRANCH(f_8C11D0)

typedef struct Result {
    uint32_t registers[8];
    Flags flags;
    uint8_t frame_bytes[0x300], stack_bytes[0x900];
} Result;

static Result invoke(uint32_t native, uint32_t caller_function, uint32_t owner,
                     uint32_t kind, uint32_t self, bool probe, bool accepted)
{
    CPU cpu = {.r = {1, native, 3, 4, SP, 5, 6, 7}, .fs_base = TLS};
    memset(MEM + SP - 0x880, 0, 0x900);
    M32(SP) = 0x0123456;
    M32(SP + 4) = STACK_REF;
    M32(SP + 12) = REGISTRY;
    M32(FRAME + 8) = native;
    M32(CALLER) = owner;
    M32(CALLER + 8) = caller_function;
    M32(CALLER + 0x14) = kind;
    M32(CALLER + 0x18) = self;
    observe = probe;
    expected_self = accepted ? self : 0;
    runs = observations = 0;
    f_C46CB0(&cpu);
    assert(cpu.r[0] == 0 && cpu.r[4] == SP + 20);
    assert(M32(TLS_DATA + 0x4ac) == 5);
    assert(runs == 1 && observations == (unsigned)(probe && accepted));
    Result result = {.flags = cpu.flags};
    memcpy(result.registers, cpu.r, sizeof result.registers);
    memcpy(result.frame_bytes, MEM + FRAME, sizeof result.frame_bytes);
    memcpy(result.stack_bytes, MEM + SP - 0x880, sizeof result.stack_bytes);
    return result;
}

int main(void)
{
    assert(gm_reserve(&space, NULL));
    const uint32_t locations[] = {TLS, SP - 0x880, SP, NATIVE_TABLE, TLS_INDEX};
    for (size_t i = 0; i < sizeof locations / sizeof locations[0]; ++i) {
        uint32_t address = locations[i];
        if (!gm_pointer(&space, address, 4))
            assert(gm_commit_page(&space, address / space.page_size * space.page_size, NULL));
    }
    MEM = space.base;
    M32(TLS + 0x2c) = TLS_TABLE;
    M32(TLS_TABLE) = TLS_DATA;
    M32(TLS_DATA + 0x4ac) = 5;
    M32(KEEP) = M32(STOP) = NATIVE_TABLE;
    M32(NATIVE_TABLE + 0x54) = SLOT_CHECK;
    M32(NATIVE_TABLE + 0x58) = SLOT_RUN;
    M32(STACK_REF) = STACK;
    M32(STACK + 0x30) = FRAME;
    M32(STACK + 0x44) = STACK_ID;
    M32(FRAME) = M32(OUTER) = STACK;
    M32(FRAME + 4) = CALLER;
    M32(CALLER + 4) = OUTER;
    M32(OUTER + 8) = MOVE_UP;
    M32(OUTER + 0x14) = EFFECT_CLASS;
    M32(OUTER + 0x18) = FIRST_SELF;

    const struct Scenario {
        uint32_t native, function, owner, kind, self;
        bool accepted;
    } scenarios[] = {
        {KEEP, MOVE_UP, STACK, EFFECT_CLASS, FIRST_SELF, true},
        {KEEP, ON_UPDATE, STACK, EFFECT_CLASS, SECOND_SELF, true},
        {STOP, MOVE_DOWN, STACK, EFFECT_CLASS, SECOND_SELF, true},
        {STOP, ON_UPDATE, STACK, EFFECT_CLASS, FIRST_SELF, false},
        {KEEP, 0, STACK, EFFECT_CLASS, FIRST_SELF, false},
        {KEEP, MOVE_UP, OTHER_STACK, EFFECT_CLASS, FIRST_SELF, false},
        {KEEP, MOVE_UP, STACK, 0, FIRST_SELF, false},
        {STOP, MOVE_DOWN, STACK, EFFECT_CLASS, 0, false},
    };
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i) {
        const struct Scenario *s = &scenarios[i];
        Result plain = invoke(s->native, s->function, s->owner, s->kind, s->self, false, s->accepted);
        Result sample = invoke(s->native, s->function, s->owner, s->kind, s->self, true, s->accepted);
        assert(!memcmp(plain.registers, sample.registers, sizeof plain.registers));
        assert(!memcmp(&plain.flags, &sample.flags, sizeof plain.flags));
        assert(!memcmp(plain.frame_bytes, sample.frame_bytes, sizeof plain.frame_bytes));
        assert(!memcmp(plain.stack_bytes, sample.stack_bytes, sizeof plain.stack_bytes));
    }
    assert(gm_release(&space, NULL));
    puts("Classic ordinary native invocation lends the nearest typed script self");
    return 0;
}
