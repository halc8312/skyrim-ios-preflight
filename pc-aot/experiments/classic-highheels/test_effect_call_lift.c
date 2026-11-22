/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "guest_memory.h"
#include "highheel_native_frame.h"
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
    ARGUMENT_PAGE = 0x1a80, ARGUMENT_PAGES = 0x1a10, PROBE_SP = 0x2b80,
    ARG_OBJECT_CLASS = 0x4a00, ARG_OBJECT = 0x4b00, VM_FLOAT = 4,
    SLOT_CHECK = 1, SLOT_RUN = 2, RUN_RETURN = 0xc46e61, STACK_ID = 73,
};

#define EPOCH UINT64_C(0x100000007)

uint8_t *MEM;
static gm_space space;
static const HighHeelNativeBindings bindings = {
    .epoch = EPOCH, .registry = REGISTRY, .effect_class = EFFECT_CLASS,
    .keep_update = KEEP, .stop_update = STOP,
    .move_up = MOVE_UP, .on_update = ON_UPDATE, .move_down = MOVE_DOWN,
};
static const CPU *current_owner;
static bool observe;
static unsigned runs, observations;
static uint32_t expected_self;
static unsigned argument_checks;

typedef struct BorrowedArguments {
    uint32_t target_class, target_identifier, magnitude_bits;
} BorrowedArguments;

/* The caller and authored VM page map were already checked. These helpers
 * run the stock getters on a separate stack in this host test. */
static uint32_t argument_page(CPU *scratch)
{
    scratch->r[1] = STACK;
    scratch->r[4] = PROBE_SP;
    M32(PROBE_SP + 4) = FRAME;
    f_C3A620(scratch);
    assert(scratch->r[4] == PROBE_SP + 8);
    return scratch->r[0];
}

static uint32_t argument_value(CPU *scratch, uint32_t index, uint32_t page)
{
    scratch->r[1] = STACK;
    scratch->r[4] = PROBE_SP;
    M32(PROBE_SP + 4) = FRAME;
    M32(PROBE_SP + 8) = index;
    M32(PROBE_SP + 12) = page;
    f_C3AC40(scratch);
    assert(scratch->r[4] == PROBE_SP + 16);
    return scratch->r[0];
}

static bool read_fixture_arguments(const CPU *cpu, BorrowedArguments *sample)
{
    bool keep = cpu->r[1] == KEEP;
    if (M32(FRAME + 0x1c) != (keep ? 2u : 1u))
        return false;
    CPU scratch = *cpu;
    uint32_t page = argument_page(&scratch);
    uint32_t reference = argument_value(&scratch, 0, page);
    assert(reference == FRAME + 0x24);
    uint32_t kind = M32(reference), identifier = M32(reference + 4);
    if (kind != ARG_OBJECT_CLASS || !identifier || !gm_pointer(&space, identifier, 0x1c))
        return false;
    uint32_t bits = 0;
    if (keep) {
        uint32_t magnitude = argument_value(&scratch, 1, page);
        assert(magnitude == FRAME + 0x2c);
        if (M32(magnitude) != VM_FLOAT)
            return false;
        bits = M32(magnitude + 4);
        if ((bits & 0x7f800000u) == 0x7f800000u)
            return false;
    }
    *sample = (BorrowedArguments){kind, identifier, bits};
    return true;
}

static void check_fixture_arguments(const CPU *cpu)
{
    bool keep = cpu->r[1] == KEEP;
    M32(FRAME + 0x24) = ARG_OBJECT_CLASS;
    M32(FRAME + 0x28) = ARG_OBJECT;
    M32(FRAME + 0x2c) = VM_FLOAT;
    M32(FRAME + 0x30) = 0xc0200000u;
    BorrowedArguments sample = {0};
    for (unsigned external = 0; external < 2; ++external) {
        M32(STACK + 0xc) = external ? 0 : 0x80000000u;
        M32(STACK + 0x10) = external ? ARGUMENT_PAGES : ARGUMENT_PAGE;
        M32(FRAME + 0x1c) = 0;
        assert(!read_fixture_arguments(cpu, &sample));
        M32(FRAME + 0x1c) = keep ? 2u : 1u;
        assert(read_fixture_arguments(cpu, &sample));
        assert(sample.target_class == ARG_OBJECT_CLASS && sample.target_identifier == ARG_OBJECT);
        assert(sample.magnitude_bits == (keep ? 0xc0200000u : 0));
        M32(FRAME + 0x24) = ARG_OBJECT_CLASS | 1u;
        assert(!read_fixture_arguments(cpu, &sample));
        M32(FRAME + 0x24) = ARG_OBJECT_CLASS;
        M32(FRAME + 0x28) = 0;
        assert(!read_fixture_arguments(cpu, &sample));
        M32(FRAME + 0x28) = ARG_OBJECT;
        M32(FRAME + 0x2c) = 3; /* Int instead of this script's declared Float. */
        assert(read_fixture_arguments(cpu, &sample) == !keep);
        M32(FRAME + 0x2c) = VM_FLOAT;
        M32(FRAME + 0x30) = 0x7fc00000u;
        assert(read_fixture_arguments(cpu, &sample) == !keep);
        M32(FRAME + 0x30) = 0xc0200000u;
    }
    /* Restore authored test inputs before returning to the original Run body. */
    M32(STACK + 0xc) = M32(STACK + 0x10) = 0;
    memset(MEM + FRAME + 0x1c, 0, 0x18);
    ++argument_checks;
}

static bool copy_guest(void *context, uint32_t address, void *value, size_t size)
{
    const void *bytes = gm_pointer(context, address, size);
    if (!bytes) return false;
    memcpy(value, bytes, size);
    return true;
}

static bool check_borrowed_self(void *context, uint32_t value)
{
    assert(context == current_owner && value == CALLER + 0x14);
    assert(M32(value) == EFFECT_CLASS && M32(value + 4) == expected_self);
    ++observations;
    return true;
}

static bool observe_caller(CPU *cpu, const HighHeelNativeBindings *auth, uint64_t epoch, uint32_t sp)
{
    return highheel_visit_native_self(auth, epoch, cpu, current_owner, cpu->r[1], sp,
                                      copy_guest, &space, check_borrowed_self, cpu);
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
        assert(!observe_caller(cpu, &bindings, (uint32_t)EPOCH, sp));
        assert(!observe_caller(cpu, &bindings, EPOCH, UINT32_MAX - 3));
        CPU foreign = *cpu;
        assert(!observe_caller(&foreign, &bindings, EPOCH, sp));
        HighHeelNativeBindings other = bindings;
        other.registry += 4;
        assert(!observe_caller(cpu, &other, EPOCH, sp));
        /* Model another invocation running on this stack. Even while the
         * outer frame is allocated, its callback is no longer current. */
        M32(STACK + 0x30) = OUTER;
        assert(!observe_caller(cpu, &bindings, EPOCH, sp));
        M32(STACK + 0x30) = FRAME;
        M32(STACK + 0x44) = STACK_ID + 1;
        assert(!observe_caller(cpu, &bindings, EPOCH, sp));
        M32(STACK + 0x44) = STACK_ID;
        M32(FRAME + 4) = FRAME;
        assert(!observe_caller(cpu, &bindings, EPOCH, sp));
        M32(FRAME + 4) = 0x100000;
        assert(!observe_caller(cpu, &bindings, EPOCH, sp));
        M32(FRAME + 4) = CALLER;
        const uint32_t result = M32(sp + 16);
        M32(sp + 16) += 4;
        assert(!observe_caller(cpu, &bindings, EPOCH, sp));
        M32(sp + 16) = result;
        const uint32_t base = M32(sp + 4);
        M32(base) = EFFECT_CLASS;
        assert(!observe_caller(cpu, &bindings, EPOCH, sp));
        M32(base) = 0;
        bool accepted = observe_caller(cpu, &bindings, EPOCH, sp);
        assert(accepted == !!expected_self);
        if (accepted)
            check_fixture_arguments(cpu);
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
    runs = observations = argument_checks = 0;
    current_owner = &cpu;
    f_C46CB0(&cpu);
    current_owner = NULL;
    assert(cpu.r[0] == 0 && cpu.r[4] == SP + 20);
    assert(M32(TLS_DATA + 0x4ac) == 5);
    assert(runs == 1 && observations == (unsigned)(probe && accepted) && argument_checks == observations);
    Result result = {.flags = cpu.flags};
    memcpy(result.registers, cpu.r, sizeof result.registers);
    memcpy(result.frame_bytes, MEM + FRAME, sizeof result.frame_bytes);
    memcpy(result.stack_bytes, MEM + SP - 0x880, sizeof result.stack_bytes);
    return result;
}

int main(void)
{
    assert(gm_reserve(&space, NULL));
    const uint32_t locations[] = {TLS, PROBE_SP, SP - 0x880, SP, NATIVE_TABLE, TLS_INDEX};
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
    M32(STACK + 0x28) = 1;
    M32(ARGUMENT_PAGES) = ARGUMENT_PAGE;
    M32(ARGUMENT_PAGE) = 0x100;
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
    puts("Classic ordinary native invocation lends typed script self and update arguments");
    return 0;
}
