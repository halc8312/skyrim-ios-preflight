/* SPDX-License-Identifier: GPL-3.0-only */
#include "form_view.h"
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    SP = 0x2f00, ID_SP = 0x2d00, ACTOR = 0x2c00, IDENTIFIER = 0x2b00,
    ACTOR_CLASS = 0x2a00, FORM_CLASS = 0x2a10, NATIVE = 0x2900, TABLE = 0x2800,
    ENTRY = 0x2780, POLICY = 0x2600, POLICY_VTABLE = 0x2620, PAGE = 0x2400,
    FRAME = PAGE + 0x10, FIRST_ARG = FRAME + 0x24, STACK = 0x2200, PAGES = 0x2260,
    REGISTRY = 0x2110, FORM_ID = 0x0a012345,
    FORM_HIGH = 0xffff, DELETED = 1 << 21, FORM_TABLE_LOCK = 0x12e59e4,
    FORM_TABLE_GLOBAL = 0x12e59d4, POLICY_GLOBAL = 0x1ba7008,
    VALUE_INITIALIZED = 0x1ba7024, SLEEP_IMPORT = 0x106b148, CAS_IMPORT = 0x106b14c,
    THREAD_IMPORT = 0x106b20c, DECREMENT_IMPORT = 0x106b154, CRC_TABLE = 0x110d240,
    INVALID_HANDLE = 0x8b4300, IS_TYPE = 0x8b3b20, IS_VALID = 0x8b3d60, RESOLVE = 0x8b3e30,
    HOST_CAS = 1, HOST_SLEEP = 2, HOST_THREAD = 3, HOST_DECREMENT = 4,
};

typedef enum FormResult { INVALID, NONE, RESOLVED_ACTOR } FormResult;

typedef struct EntrySample {
    uint32_t key, form_id, flags;
} EntrySample;

uint8_t *MEM;
static gm_space space;
static const CPU *invoker;
static unsigned locks, handle_count, entry_count;
static uint64_t handles[2];
static EntrySample entries[2];
static FormResult result;
static uint32_t result_id, unpacked_type;
static bool stock_resolved, replace_during_resolve;

void f_8F8120(CPU *);
void f_8D90E0(CPU *);
void f_8FCD50(CPU *);
void f_8B4300(CPU *);
void f_8B3B20(CPU *);
void f_8B3D60(CPU *);
void f_8B3E30(CPU *);

#define UNREACHABLE(name) \
    void name(CPU *cpu) { (void)cpu; assert(0 && "unexpected branch in stock Form argument"); }
UNREACHABLE(f_8B3900)
UNREACHABLE(f_8B3A50)
UNREACHABLE(f_56B6C0)
UNREACHABLE(f_5509E0)
UNREACHABLE(f_663410)
UNREACHABLE(f_F520BC)

void rt_call(CPU *cpu, uint32_t target)
{
    (void)cpu;
    (void)target;
    assert(0 && "unexpected indirect target");
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    uint32_t sp = cpu->r[4], site = M32(sp);
    switch (target) {
    case INVALID_HANDLE:
        f_8B4300(cpu);
        return;
    case IS_TYPE:
        f_8B3B20(cpu);
        return;
    case IS_VALID:
        f_8B3D60(cpu);
        return;
    case RESOLVE:
        f_8B3E30(cpu);
        return;
    case HOST_THREAD:
        assert(cpu == invoker && site == 0xa4aee9 && !M32(FORM_TABLE_LOCK));
        if (++locks == 2 && replace_during_resolve)
            M32(ACTOR + 0x8) = DELETED; /* Authored change between the two stock lookups. */
        cpu->r[0] = 1;
        cpu->r[4] += 4;
        return;
    case HOST_DECREMENT:
        assert(cpu == invoker && site == 0xa4b0f7 && M32(sp + 4) == FORM_TABLE_LOCK);
        assert(M32(FORM_TABLE_LOCK) == 1);
        M32(FORM_TABLE_LOCK) = 0;
        cpu->r[0] = 0;
        cpu->r[4] += 8;
        return;
    case HOST_CAS: {
        uint32_t lock = M32(sp + 4), desired = M32(sp + 8);
        uint32_t expected = M32(sp + 12);
        assert(cpu == invoker);
        if (site == 0xa4af0f) {
            assert(lock == FORM_TABLE_LOCK && expected == 0 && desired == 1);
        } else {
            assert(site == 0xc3080e && lock == IDENTIFIER + 0x18);
            assert(desired == (expected | UINT32_C(0x80000000)));
        }
        assert(expected == M32(lock));
        M32(lock) = desired;
        cpu->r[0] = expected;
        cpu->r[4] += 16;
        return;
    }
    case HOST_SLEEP:
        assert(0 && "authored identifier lock must be available");
        return;
    default:
        assert(0 && "unexpected virtual call");
    }
}

bool classic_engine_read_data(const CPU *cpu, uint32_t address, uint8_t *bytes, size_t count)
{
    assert(cpu == invoker && M32(FORM_TABLE_LOCK) == 1);
    const void *source = gm_pointer(&space, address, count);
    if (!source)
        return false;
    memcpy(bytes, source, count);
    return true;
}

void fixture_form_handle(const CPU *cpu, uint32_t low, uint32_t high)
{
    assert(cpu == invoker && handle_count < 2);
    assert(M32(IDENTIFIER + 0x18) == UINT32_C(0x80000002));
    handles[handle_count++] = (uint64_t)high << 32 | low;
}

void fixture_form_entry(const CPU *cpu, uint32_t site, uint32_t key, uint32_t form)
{
    assert(cpu == invoker && M32(FORM_TABLE_LOCK) == 1 && entry_count < 2);
    assert(site == (entry_count ? 0x8b3eb0u : 0x8b3dbeu));
    assert(form == ACTOR && gm_pointer(&space, form, 0x13));
    EntrySample sample = {.key = key, .flags = M32(form + 0x8)};
    (void)classic_read_form_id(cpu, form, CLASSIC_CHARACTER_FORM, &sample.form_id);
    entries[entry_count++] = sample;
}

void fixture_unpack_form(CPU *cpu)
{
    uint32_t sp = cpu->r[4], value = M32(sp + 4);
    assert(cpu == invoker && M32(sp) == 0x8f814d && value == FIRST_ARG);
    assert(gm_pointer(&space, value, 8));
    unpacked_type = M32(value);
    uint32_t identifier = M32(value + 4);
    if (unpacked_type >= 16 && !(unpacked_type & 1u) &&
        identifier && (identifier != IDENTIFIER || !gm_pointer(&space, identifier, 0x1c))) {
        /* An authored sandbox admission guard. Do not dereference a forged identifier. */
        cpu->r[0] = 0;
        cpu->r[4] += 4;
        return;
    }
    f_8D90E0(cpu);
}

/* The stock Run continues here into FormList's base/return handling.
 * This replacement consumes the ordinary argument while the call is active. */
void f_8F7C80(CPU *cpu)
{
    uint32_t sp = cpu->r[4], form = M32(sp + 12);
    assert(M32(sp) == 0x8f816e && M32(sp + 8) == 0x8f78c0 && M32(sp + 16) == REGISTRY);
    assert(cpu == invoker && !M32(FORM_TABLE_LOCK));
    stock_resolved = form != 0;
    if (unpacked_type == 0 && !form) {
        result = NONE;
    } else if (form && (unpacked_type == ACTOR_CLASS || unpacked_type == FORM_CLASS) &&
               handle_count == 2 && handles[0] == handles[1] && entry_count == 2) {
        uint32_t handle_id = (uint32_t)handles[1];
        const EntrySample *before = &entries[0], *after = &entries[1];
        if (handle_id && before->key == handle_id && after->key == handle_id &&
            before->form_id == handle_id && after->form_id == handle_id &&
            !((before->flags | after->flags) & DELETED)) {
            result_id = after->form_id;
            result = RESOLVED_ACTOR;
            /* Cross-check the scalar with stock Form.GetFormID in the fixed hosted scene. */
            CPU getter = *cpu;
            getter.r[4] = ID_SP;
            M32(ID_SP + 12) = form;
            f_8FCD50(&getter);
            assert(getter.r[0] == result_id && getter.r[4] == ID_SP + 4);
        }
    }
    cpu->r[0] = 1;
    cpu->r[4] += 4;
}

typedef struct Scenario {
    const char *label;
    uint32_t kind, identifier, hi, id, flags;
    uint8_t form_type;
    bool missing_argument, missing_form, external_pages, deleted_during_resolve, stock_resolves;
    FormResult expected;
} Scenario;

static void run(const Scenario *s)
{
    CPU cpu = {.r = {0, NATIVE, 0, 0, SP}};
    M32(SP + 4) = FRAME + 0x14;
    M32(SP + 8) = REGISTRY;
    M32(SP + 12) = 1;
    M32(SP + 16) = FRAME + 0x14;
    M32(SP + 20) = FRAME;
    M32(STACK + 0x0c) = s->external_pages ? 0 : UINT32_C(0x80000000);
    M32(STACK + 0x10) = s->external_pages ? PAGES : PAGE;
    M32(FRAME + 0x1c) = s->missing_argument ? 0 : 1;
    M32(FIRST_ARG) = s->kind;
    M32(FIRST_ARG + 4) = s->identifier;
    M32(IDENTIFIER + 0x10) = FORM_ID;
    M32(IDENTIFIER + 0x14) = s->hi;
    M32(IDENTIFIER + 0x18) = 2;
    M32(ENTRY + 8) = s->missing_form ? 0 : 1;
    M32(ACTOR + 0xc) = s->id;
    M32(ACTOR + 0x8) = s->flags;
    M8(ACTOR + 0x12) = s->form_type;
    replace_during_resolve = s->deleted_during_resolve;
    invoker = &cpu;
    result = INVALID;
    unpacked_type = UINT32_MAX;
    stock_resolved = false;
    result_id = locks = handle_count = entry_count = 0;
    f_8F8120(&cpu);
    invoker = NULL;
    if (result != s->expected || stock_resolved != s->stock_resolves)
        fprintf(stderr, "unexpected result for %s\n", s->label);
    assert(result == s->expected && stock_resolved == s->stock_resolves);
    assert(result_id == (result == RESOLVED_ACTOR ? FORM_ID : 0));
    assert(!M32(FORM_TABLE_LOCK) && cpu.r[4] == SP + 24 && M32(IDENTIFIER + 0x18) == 2);
    assert(M32(FIRST_ARG) == s->kind && M32(FIRST_ARG + 4) == s->identifier);
    assert(M32(ACTOR + 0xc) == s->id && M32(IDENTIFIER + 0x14) == s->hi);
}

int main(void)
{
    assert(gm_reserve(&space, NULL));
    const uint32_t locations[] = {SP, CAS_IMPORT, POLICY_GLOBAL, FORM_TABLE_GLOBAL, CRC_TABLE};
    for (size_t i = 0; i < sizeof locations / sizeof locations[0]; ++i) {
        uint32_t address = locations[i];
        if (!gm_pointer(&space, address, 4))
            assert(gm_commit_page(&space, address / space.page_size * space.page_size, NULL));
    }
    MEM = space.base;
    M32(SLEEP_IMPORT) = HOST_SLEEP;
    M32(CAS_IMPORT) = HOST_CAS;
    M32(THREAD_IMPORT) = HOST_THREAD;
    M32(DECREMENT_IMPORT) = HOST_DECREMENT;
    M32(POLICY_GLOBAL) = POLICY;
    M32(POLICY) = POLICY_VTABLE;
    M32(POLICY_VTABLE + 0x4) = IS_TYPE;
    M32(POLICY_VTABLE + 0x8) = IS_VALID;
    M32(POLICY_VTABLE + 0xc) = INVALID_HANDLE;
    M32(POLICY_VTABLE + 0x20) = RESOLVE;
    M32(VALUE_INITIALIZED) = 1;
    M32(FORM_TABLE_GLOBAL) = TABLE;
    M32(TABLE + 0x8) = 1;
    M32(TABLE + 0x1c) = ENTRY;
    M32(ENTRY) = FORM_ID;
    M32(ENTRY + 4) = ACTOR;
    M32(NATIVE + 0x2c) = 0x8f78c0;
    M32(FRAME) = STACK;
    M32(STACK + 0x28) = 1;
    M32(PAGES) = PAGE;
    M32(PAGE) = 0x100;
    const Scenario live = {
        .label = "Actor held as a Form argument", .kind = ACTOR_CLASS, .identifier = IDENTIFIER,
        .hi = FORM_HIGH, .id = FORM_ID, .form_type = CLASSIC_CHARACTER_FORM,
        .stock_resolves = true, .expected = RESOLVED_ACTOR,
    };
    run(&live);
    Scenario s = live;
    s.label = "generic Form tag with Actor value and external page table";
    s.kind = FORM_CLASS;
    s.external_pages = true;
    run(&s);
    s = live;
    s.label = "explicit None";
    s.kind = 0;
    s.identifier = 0;
    s.stock_resolves = false;
    s.expected = NONE;
    run(&s);
    s.label = "argument missing";
    s.missing_argument = true;
    s.expected = INVALID;
    run(&s);
    s = live;
    s.label = "typed null identifier";
    s.identifier = 0;
    s.stock_resolves = false;
    s.expected = INVALID;
    run(&s);
    s.label = "forged unmapped identifier";
    s.identifier = 0x7100;
    run(&s);
    s.label = "array tag";
    s.identifier = IDENTIFIER;
    s.kind = ACTOR_CLASS | 1u;
    run(&s);
    s = live;
    s.label = "wrong handle kind";
    s.hi = 0x10;
    s.stock_resolves = false;
    s.expected = INVALID;
    run(&s);
    s.label = "deleted form";
    s.hi = FORM_HIGH;
    s.flags = DELETED;
    run(&s);
    s.label = "absent FormID";
    s.flags = 0;
    s.missing_form = true;
    run(&s);
    s = live;
    s.label = "wrong FormID behind table entry";
    s.id = FORM_ID + 1;
    s.expected = INVALID;
    run(&s);
    s.label = "a valid non-Actor form";
    s.id = FORM_ID;
    s.form_type = 0x2b;
    run(&s);
    s.label = "form becomes deleted between validity and resolution";
    s.form_type = CLASSIC_CHARACTER_FORM;
    s.deleted_during_resolve = true;
    run(&s);
    assert(gm_release(&space, NULL));
    puts("Classic Form argument distinguishes None and verifies live Actor FormID");
    return 0;
}
