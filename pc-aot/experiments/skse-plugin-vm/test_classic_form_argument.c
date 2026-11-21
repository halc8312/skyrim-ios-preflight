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
    REGISTRY = 0x2110, FORM_ID = 0x0a012345, BASE_VALUE = FRAME + 0x14,
    KEYWORD_CLASS = 0x2a20, KEYWORD_TYPE = 0x04, KEYWORD_ID = 0x4a01d9a9, OTHER_KEYWORD_ID = 0x4b01d9a9,
    OTHER_KEYWORD = 0x2b80, OTHER_ENTRY = ENTRY + 12, STACK_ID = 9, RESULT = 0x2e00,
    SCOPE_EPOCH = 17, AUDITED_LOOKUP = 0x10000,
    KEYWORD_TYPE_SELECTOR = 0x8b3d24, KEYWORD_TYPE_DESTINATION = 0x8b3d10, KEYWORD_TYPE_FALLBACK = 0x8b3c78,
    FORM_HIGH = 0xffff, DELETED = 1 << 21, FORM_TABLE_LOCK = 0x12e59e4,
    FORM_TABLE_GLOBAL = 0x12e59d4, POLICY_GLOBAL = 0x1ba7008,
    VALUE_INITIALIZED = 0x1ba7024, SLEEP_IMPORT = 0x106b148, CAS_IMPORT = 0x106b14c,
    THREAD_IMPORT = 0x106b20c, DECREMENT_IMPORT = 0x106b154, CRC_TABLE = 0x110d240,
    INVALID_HANDLE = 0x8b4300, IS_TYPE = 0x8b3b20, IS_VALID = 0x8b3d60, RESOLVE = 0x8b3e30,
    HOST_CAS = 1, HOST_SLEEP = 2, HOST_THREAD = 3, HOST_DECREMENT = 4,
};

typedef enum FormResult { INVALID, NONE, RESOLVED_ACTOR, RESOLVED_KEYWORD } FormResult;

typedef struct EntrySample {
    uint32_t key, form_id, flags;
} EntrySample;

uint8_t *MEM;
static gm_space space;
static const CPU *invoker;
static unsigned locks, handle_count, entry_count;
static uint64_t handles[2];
static EntrySample entries[5];
static FormResult result;
static uint32_t result_id, unpacked_type;
static bool stock_resolved, replace_during_resolve;
static bool keyword_trial, typed_keyword;
static uint32_t scoped_value, scoped_epoch;

void f_8F8120(CPU *);
void f_8D90E0(CPU *);
void f_8FCD50(CPU *);
void f_451A30(CPU *);
void f_8E0040(CPU *);
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
    const uint32_t generic_sites[] = {0x8b3dbe, 0x8b3eb0, AUDITED_LOOKUP};
    const uint32_t typed_sites[] = {0x8b3c26, 0x8b3dbe, 0x8b3c26, 0x8b3eb0, AUDITED_LOOKUP};
    const unsigned max_reads = typed_keyword ? 5 : keyword_trial ? 3 : 2;
    assert(cpu == invoker && M32(FORM_TABLE_LOCK) == 1 && entry_count < max_reads);
    assert(site == (typed_keyword ? typed_sites : generic_sites)[entry_count]);
    assert((form == ACTOR || (keyword_trial && form == OTHER_KEYWORD)) && gm_pointer(&space, form, 0x13));
    EntrySample sample = {.key = key, .flags = M32(form + 0x8)};
    const uint8_t expected_type = keyword_trial ? KEYWORD_TYPE : CLASSIC_CHARACTER_FORM;
    (void)classic_read_form_id(cpu, form, expected_type, &sample.form_id);
    entries[entry_count++] = sample;
}

void fixture_unpack_form(CPU *cpu)
{
    uint32_t sp = cpu->r[4], value = M32(sp + 4);
    assert(cpu == invoker && M32(sp) == 0x8f814d && value == FIRST_ARG);
    assert(gm_pointer(&space, value, 8));
    unpacked_type = M32(value);
    if (keyword_trial && M32(sp + 8) == REGISTRY) scoped_value = value;
    uint32_t identifier = M32(value + 4);
    if (unpacked_type >= 16 && !(unpacked_type & 1u) &&
        identifier && (identifier != IDENTIFIER || !gm_pointer(&space, identifier, 0x1c))) {
        /* An authored sandbox admission guard. Do not dereference a forged identifier. */
        cpu->r[0] = 0;
        cpu->r[4] += 4;
        return;
    }
    if (typed_keyword) f_8E0040(cpu);
    else f_8D90E0(cpu);
}

/* Only a copied numeric identity can leave this original wrapper frame. */
static uint32_t project_keyword(CPU *cpu, uint32_t sp, uint32_t supplied)
{
    const unsigned native_reads = typed_keyword ? 4 : 2;
    if (!supplied || cpu != invoker || scoped_value != FIRST_ARG || scoped_epoch != SCOPE_EPOCH ||
        M32(sp + 4) != BASE_VALUE || M32(sp + 16) != REGISTRY ||
        M32(sp + 20) != STACK_ID || M32(sp + 24) != RESULT ||
        M32(SP + 20) != FRAME || M32(FRAME) != STACK ||
        (unpacked_type != KEYWORD_CLASS && unpacked_type != FORM_CLASS) ||
        M32(FIRST_ARG) != unpacked_type || M32(FIRST_ARG + 4) != IDENTIFIER ||
        handle_count != 2 || handles[0] != handles[1] || entry_count != native_reads) return 0;

    /* Use the same guest CPU in the current native invocation. The original
     * registry lookup acquires and releases its own scoped read guard. */
    const CPU caller = *cpu;
    cpu->r[4] = ID_SP;
    M32(ID_SP) = AUDITED_LOOKUP;
    M32(ID_SP + 4) = KEYWORD_ID;
    f_451A30(cpu);
    const bool same = cpu->r[0] == supplied;
    assert(cpu->r[4] == ID_SP + 4);
    *cpu = caller;
    if (!same || entry_count != native_reads + 1 || handles[1] != ((uint64_t)FORM_HIGH << 32 | KEYWORD_ID)) return 0;
    for (unsigned i = 0; i < entry_count; ++i)
        if (entries[i].key != KEYWORD_ID || entries[i].form_id != KEYWORD_ID || (entries[i].flags & DELETED))
            return 0;
    return entries[native_reads].form_id;
}

/* The stock Run continues here into FormList's base/return handling.
 * This replacement consumes the ordinary argument while the call is active. */
void f_8F7C80(CPU *cpu)
{
    uint32_t sp = cpu->r[4], form = M32(sp + 12);
    assert(M32(sp) == 0x8f816e && M32(sp + 8) == 0x8f78c0);
    assert(keyword_trial || M32(sp + 16) == REGISTRY);
    assert(cpu == invoker && !M32(FORM_TABLE_LOCK));
    stock_resolved = form != 0;
    if (keyword_trial) {
        result_id = project_keyword(cpu, sp, form);
        if (result_id) result = RESOLVED_KEYWORD;
    } else if (unpacked_type == 0 && !form) {
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
    uint32_t keyword_handle;
    bool wrong_registry, inactive_epoch;
    uint8_t form_type;
    bool missing_argument, missing_form, external_pages, deleted_during_resolve, stock_resolves;
    FormResult expected;
} Scenario;

static void run(const Scenario *s)
{
    CPU cpu = {.r = {0, NATIVE, 0, 0, SP}};
    keyword_trial = s->keyword_handle != 0;
    scoped_value = 0;
    scoped_epoch = s->inactive_epoch ? SCOPE_EPOCH - 1 : SCOPE_EPOCH;
    M32(SP + 4) = FRAME + 0x14;
    M32(SP + 8) = s->wrong_registry ? REGISTRY + 4 : REGISTRY;
    M32(SP + 12) = STACK_ID;
    M32(SP + 16) = RESULT;
    M32(SP + 20) = FRAME;
    M32(STACK + 0x0c) = s->external_pages ? 0 : UINT32_C(0x80000000);
    M32(STACK + 0x10) = s->external_pages ? PAGES : PAGE;
    M32(FRAME + 0x1c) = s->missing_argument ? 0 : 1;
    M32(FIRST_ARG) = s->kind;
    M32(FIRST_ARG + 4) = s->identifier;
    M32(IDENTIFIER + 0x10) = keyword_trial ? s->keyword_handle : FORM_ID;
    M32(IDENTIFIER + 0x14) = s->hi;
    M32(IDENTIFIER + 0x18) = 2;
    M32(ENTRY) = keyword_trial ? KEYWORD_ID : FORM_ID;
    M32(ENTRY + 8) = s->missing_form ? 0 : keyword_trial ? OTHER_ENTRY : 1;
    M32(OTHER_ENTRY) = OTHER_KEYWORD_ID;
    M32(OTHER_ENTRY + 4) = OTHER_KEYWORD;
    M32(OTHER_ENTRY + 8) = 1;
    M32(OTHER_KEYWORD + 0x8) = 0;
    M32(OTHER_KEYWORD + 0xc) = OTHER_KEYWORD_ID;
    M8(OTHER_KEYWORD + 0x12) = KEYWORD_TYPE;
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
    scoped_value = 0;
    if (result != s->expected || stock_resolved != s->stock_resolves)
        fprintf(stderr, "unexpected result for %s: projection=%d stock=%u table_hits=%u handle_reads=%u\n",
                s->label, (int)result, (unsigned)stock_resolved, entry_count, handle_count);
    assert(result == s->expected && stock_resolved == s->stock_resolves);
    const uint32_t expected_id = result == RESOLVED_KEYWORD ? KEYWORD_ID : result == RESOLVED_ACTOR ? FORM_ID : 0;
    assert(result_id == expected_id);
    assert(!M32(FORM_TABLE_LOCK) && cpu.r[4] == SP + 24 && M32(IDENTIFIER + 0x18) == 2);
    assert(M32(FIRST_ARG) == s->kind && M32(FIRST_ARG + 4) == s->identifier);
    assert(M32(ACTOR + 0xc) == s->id && M32(IDENTIFIER + 0x14) == s->hi);
}

static void wearable_keyword_cases(void)
{
    const Scenario keyword = {
        .label = "audited Wearable Lanterns Keyword in the owned VM frame",
        .kind = KEYWORD_CLASS, .identifier = IDENTIFIER, .hi = FORM_HIGH,
        .id = KEYWORD_ID, .form_type = KEYWORD_TYPE, .keyword_handle = KEYWORD_ID,
        .stock_resolves = true, .expected = RESOLVED_KEYWORD,
    };
    run(&keyword);
    Scenario s = keyword;
    s.label = "same Keyword passed as Form via the external argument page";
    s.kind = FORM_CLASS;
    s.external_pages = true;
    run(&s);
    s = keyword;
    s.label = "equal local FormID in another plugin";
    s.keyword_handle = OTHER_KEYWORD_ID;
    s.expected = INVALID;
    run(&s);
    s = keyword;
    s.label = "matching registry pointer and ID, but not a Keyword";
    s.form_type = CLASSIC_CHARACTER_FORM;
    s.stock_resolves = !typed_keyword;
    s.expected = INVALID;
    run(&s);
    s = keyword;
    s.label = "the engine's type policy alias is not an exact Keyword";
    s.form_type = 5; /* LocationRefType falls back through the policy for 4. */
    s.expected = INVALID;
    run(&s);
    s = keyword;
    s.label = "matching registry pointer and type, but mismatched stored FormID";
    s.id ^= 1;
    s.expected = INVALID;
    run(&s);
    s = keyword;
    s.label = "Keyword array instead of scalar argument";
    s.kind |= 1;
    s.stock_resolves = false;
    s.expected = INVALID;
    run(&s);
    s = keyword;
    s.label = "typed empty Keyword";
    s.identifier = 0;
    s.stock_resolves = false;
    s.expected = INVALID;
    run(&s);
    s = keyword;
    s.label = "right Keyword outside the accepted registry";
    s.wrong_registry = true;
    s.expected = INVALID;
    run(&s);
    s = keyword;
    s.label = "right Keyword in an already stale VM epoch";
    s.inactive_epoch = true;
    s.expected = INVALID;
    run(&s);
}

int main(int argc, char **argv)
{
    assert(gm_reserve(&space, NULL));
    const uint32_t locations[] = {SP, CAS_IMPORT, POLICY_GLOBAL, FORM_TABLE_GLOBAL, CRC_TABLE, KEYWORD_TYPE_SELECTOR};
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
    /* From the reviewed LE input: requested type 4 picks slot zero when the
     * stored form type has already failed the exact comparison. */
    M8(KEYWORD_TYPE_SELECTOR) = 0;
    M32(KEYWORD_TYPE_DESTINATION) = KEYWORD_TYPE_FALLBACK;
    if (argc == 2 && (!strcmp(argv[1], "wearable-keyword") || !strcmp(argv[1], "wearable-typed-keyword"))) {
        typed_keyword = !strcmp(argv[1], "wearable-typed-keyword");
        wearable_keyword_cases();
        assert(gm_release(&space, NULL));
        puts("Classic Form argument joins the audited Wearable Lanterns Keyword");
        return 0;
    }
    assert(argc == 1);
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
