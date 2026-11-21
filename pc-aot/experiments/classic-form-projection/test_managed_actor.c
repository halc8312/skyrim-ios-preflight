/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(SKYRIM_TEST_PROJECT_FORM)
#define MEM classic_managed_actor_memory
#define rt_call_ic classic_managed_actor_call_ic
#define f_C398A0 classic_managed_actor_log_unreachable
#define classic_engine_read_data classic_managed_actor_read_data
#define classic_read_form_id classic_managed_actor_read_form_id
#endif
#include "lift_prelude.h"
#include "form_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

enum {
    GUEST_BYTES = 0x1bc0000,
    DATA = 0x10000,
    FS = DATA,
    TLS = DATA + 0x100,
    TLS_SLOTS = DATA + 0x200,
    STACK = 0x20000,
    SCRIPT_STACK = DATA + 0x1000,
    STACK_REF = DATA + 0x1100,
    LIST = DATA + 0x1200,
    BLOCK_INDEX = DATA + 0x1300,
    BLOCK = DATA + 0x2000,
    STATE = BLOCK + 0x20,
    ARG_VALUE = STATE + 0x24,
    BASE_VALUE = STATE + 0x14,
    BASE_ID = DATA + 0x1400,
    ARG_ID = DATA + 0x1500,
    FUNCTION = DATA + 0x1600,
    REGISTRY = DATA + 0x1700,
    POLICY = DATA + 0x1800,
    CLASS = DATA + 0x1900,
    ACTORS = 0x30000,
    BASE_ACTOR = ACTORS + 0x100,
    ACTOR = ACTORS + 0x200,
    OTHER = ACTORS + 0x300,
    FORM_ID = 0x1234567,
    ID_TAG = 0x357ad,
    STACK_ID = 0x23456,
    EPOCH = 45,
    POLICY_ROOT = 0x1ba7008,
    TLS_INDEX = 0x1bbeb54,
    NATIVE_TABLE = 0x10ee2e4,
    POLICY_TABLE = 0x10eba1c,
    INVOKE = 0xc46cb0,
    RUN = 0x8e5b90,
    CALLBACK = 0x8db8c0,
    HAS_CALLBACK = 0x90b480,
    IS_TYPE = 0x8b3b20,
    IS_VALID = 0x8b3d60,
    RESOLVE = 0x8b3e30,
    INVALID = 0x8b4300,
    COMPARE_EXCHANGE = 0x106b14c,
};

enum Scenario {
    MATCHED, NO_ARGUMENT, PRIMITIVE, NULL_ID, WRONG_POLICY_TYPE, WRONG_ACTOR,
    WRONG_ADDRESS, NO_REFERENCE, WRONG_REGISTRY, RETIRED,
};

#define MATCH_RESULT UINT32_C(0xfe123456)
#define EMPTY_RESULT UINT32_C(0x44445555)

typedef struct { uint32_t id; uint8_t type; } ActorFields;
typedef struct {
    const CPU *cpu;
    unsigned epoch;
    bool owns_live_reference;
    ActorFields copied;
} ActorScope;

uint8_t *MEM;
static size_t page_size;
static unsigned live_epoch, field_reads, native_calls, destroys;
static uint32_t argument_result, argument_resolved;
static uint32_t expected_actor_id = FORM_ID;
static unsigned expected_epoch = EPOCH;
static bool actor_in_use;
static enum Scenario scenario;
static _Thread_local ActorScope *active;

void f_C46CB0(CPU *);
void f_8E5B90(CPU *);

static void protect(uint32_t address, int mode) {
    CHECK(!mprotect(MEM + address / page_size * page_size, page_size, mode));
}

bool classic_engine_read_data(const CPU *cpu, uint32_t address, uint8_t *bytes, size_t count) {
    CHECK(active && active->cpu == cpu && actor_in_use && active->owns_live_reference);
    CHECK(address == ACTOR + 0x0c && count == 7);
    ++field_reads;
    memcpy(bytes, MEM + address, count);
    return true;
}

/* An authoritative lease is a fixture input. The VM's pointer and matching
 * class tag alone cannot mint that lease. */
void classic_test_managed_actor(const CPU *cpu, uint32_t native, uint32_t state,
                                uint32_t registry, uint32_t value, uint32_t address) {
    CHECK(native == FUNCTION && state == STATE && value == ARG_VALUE);
    if (!active || cpu != active->cpu || !active->owns_live_reference ||
        active->epoch != live_epoch || registry != REGISTRY ||
        address != ACTOR || argument_resolved != ARG_ID ||
        M32(value) != CLASS || M32(value + 4) != ARG_ID) return;
    uint32_t copied_id;
    if (classic_read_form_id(cpu, address, CLASSIC_CHARACTER_FORM, &copied_id) && copied_id == expected_actor_id)
        active->copied = (ActorFields){copied_id, CLASSIC_CHARACTER_FORM};
}

static uint32_t handle_id(uint32_t low, uint32_t high) {
    CHECK(high == ID_TAG && (low == ARG_ID || low == BASE_ID));
    return low;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    CHECK(cache && !*cache);
    const uint32_t sp = cpu->r[4];
    const uint32_t ret = M32(sp);
    uint32_t id;
    switch (target) {
    case HAS_CALLBACK:
        CHECK(ret == 0xc46d37 && cpu->r[1] == FUNCTION);
        cpu->r[0] = 1;
        cpu->r[4] += 4;
        return;
    case RUN:
        CHECK(ret == 0xc46e61 && cpu->r[1] == FUNCTION && M32(sp + 20) == STATE);
        CHECK(M32(sp + 8) == REGISTRY + (scenario == WRONG_REGISTRY ? 4 : 0));
        CHECK(M32(sp + 12) == STACK_ID);
        CHECK(M32(M32(sp + 4)) == CLASS && M32(M32(sp + 4) + 4) == BASE_ID);
        f_8E5B90(cpu);
        CHECK((uint8_t)cpu->r[0] == 1 && cpu->r[4] == sp + 24);
        return;
    case INVALID:
        CHECK(ret == 0xc307d8 && cpu->r[1] == POLICY);
        cpu->r[0] = cpu->r[2] = UINT32_MAX;
        cpu->r[4] += 4;
        return;
    case COMPARE_EXCHANGE:
        CHECK(ret == 0xc3080e);
        CHECK(M32(sp + 4) == ARG_ID + 0x18 || M32(sp + 4) == BASE_ID + 0x18);
        CHECK(M32(sp + 8) == UINT32_C(0x80000002) && M32(sp + 12) == 2);
        CHECK(M32(M32(sp + 4)) == 2);
        M32(M32(sp + 4)) = M32(sp + 8);
        cpu->r[0] = 2;
        cpu->r[4] += 16;
        return;
    case IS_TYPE:
        CHECK(ret == 0x8d94fc && cpu->r[1] == POLICY && M32(sp + 4) == CLASSIC_CHARACTER_FORM);
        id = handle_id(M32(sp + 8), M32(sp + 12));
        cpu->r[0] = id != ARG_ID || scenario != WRONG_POLICY_TYPE;
        cpu->r[4] += 16;
        return;
    case IS_VALID:
        CHECK(ret == 0x8d955a && cpu->r[1] == POLICY);
        handle_id(M32(sp + 4), M32(sp + 8));
        cpu->r[0] = 1;
        cpu->r[4] += 12;
        return;
    case RESOLVE:
        CHECK(ret == 0x8d956f && cpu->r[1] == POLICY && M32(sp + 4) == CLASSIC_CHARACTER_FORM);
        id = handle_id(M32(sp + 8), M32(sp + 12));
        if (id == ARG_ID) argument_resolved = id;
        cpu->r[0] = id == BASE_ID ? BASE_ACTOR : argument_result;
        cpu->r[4] += 16;
        return;
    case CALLBACK:
        CHECK(ret == 0x8e1d5d && actor_in_use && M32(sp + 8) == STACK_ID);
        CHECK(M32(sp + 12) == BASE_ACTOR);
        CHECK(M32(sp + 16) == argument_result);
        ++native_calls;
        if (scenario == RETIRED) ++live_epoch;
        cpu->r[0] = argument_result ? MATCH_RESULT : EMPTY_RESULT;
        cpu->r[4] += 4;
        return;
    default:
        CHECK(false);
    }
}

/* Only the VMValue set/destroy services are authored. These assertions witness
 * the base copy, typed integer publication and ordinary VM cleanup. */
void f_C329E0(CPU *cpu) {
    const uint32_t dest = cpu->r[1], src = M32(cpu->r[4] + 4);
    switch (M32(cpu->r[4])) {
    case 0xc46d2e:
        CHECK(src == BASE_VALUE && M32(src) == CLASS && M32(src + 4) == BASE_ID);
        break;
    case 0x8e1d7a:
        CHECK(M32(src) == 3 && M32(src + 4) == (argument_result ? MATCH_RESULT : EMPTY_RESULT));
        break;
    case 0xc46e7d:
        CHECK(dest == SCRIPT_STACK + 0x3c && M32(src) == 3 && native_calls == 1);
        break;
    default:
        CHECK(false);
    }
    CHECK(M32(dest) == 0);
    M32(dest) = M32(src);
    M32(dest + 4) = M32(src + 4);
    cpu->r[4] += 8;
}

void f_C328E0(CPU *cpu) {
    const uint32_t ret = M32(cpu->r[4]), value = cpu->r[1];
    CHECK(ret == 0x8e1d83 || ret == 0xc46e1c || ret == 0xc46e25);
    CHECK(M32(value) == (ret == 0xc46e1c ? CLASS : 3));
    ++destroys;
    M32(value) = M32(value + 4) = 0;
    cpu->r[4] += 4;
}

#define UNREACHED(name) void name(CPU *cpu) { (void)cpu; CHECK(false); }
UNREACHED(f_C4D650)
UNREACHED(f_505D00)
UNREACHED(f_4202A0)
UNREACHED(f_C395D0)
UNREACHED(f_C398A0)
UNREACHED(f_8C11D0)
UNREACHED(f_F520BC)

static void set_up(void) {
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page_size >= 4096 && GUEST_BYTES % page_size == 0 && ACTORS % page_size == 0);
    MEM = mmap(NULL, GUEST_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(MEM != MAP_FAILED);
    protect(DATA, PROT_READ | PROT_WRITE);
    protect(SCRIPT_STACK, PROT_READ | PROT_WRITE);
    protect(BLOCK, PROT_READ | PROT_WRITE);
    protect(STACK, PROT_READ | PROT_WRITE);
    protect(ACTORS, PROT_READ | PROT_WRITE);
    protect(POLICY_ROOT, PROT_READ | PROT_WRITE);
    protect(TLS_INDEX, PROT_READ | PROT_WRITE);
    protect(NATIVE_TABLE, PROT_READ | PROT_WRITE);
    protect(POLICY_TABLE, PROT_READ | PROT_WRITE);
    protect(COMPARE_EXCHANGE, PROT_READ | PROT_WRITE);

    M32(TLS_INDEX) = 0;
    M32(FS + 0x2c) = TLS_SLOTS;
    M32(TLS_SLOTS) = TLS;
    M32(TLS + 0x4ac) = 7;
    M32(COMPARE_EXCHANGE) = COMPARE_EXCHANGE;
    M32(NATIVE_TABLE + 15 * 4) = INVOKE;
    M32(NATIVE_TABLE + 21 * 4) = HAS_CALLBACK;
    M32(NATIVE_TABLE + 22 * 4) = RUN;
    M32(POLICY_TABLE + 1 * 4) = IS_TYPE;
    M32(POLICY_TABLE + 2 * 4) = IS_VALID;
    M32(POLICY_TABLE + 3 * 4) = INVALID;
    M32(POLICY_TABLE + 8 * 4) = RESOLVE;
    protect(NATIVE_TABLE, PROT_READ);
    protect(POLICY_TABLE, PROT_READ);
    protect(COMPARE_EXCHANGE, PROT_READ);
    M32(FUNCTION) = NATIVE_TABLE;
    M32(FUNCTION + 0x2c) = CALLBACK;
    M32(POLICY_ROOT) = POLICY;
    M32(POLICY_ROOT + 0x1c) = 1;
    M32(POLICY) = POLICY_TABLE;
    M32(STACK_REF) = SCRIPT_STACK;
    M32(SCRIPT_STACK + 0x30) = STATE;
    M32(SCRIPT_STACK + 0x44) = STACK_ID;
    M32(LIST + 0x10) = BLOCK_INDEX;
    M32(LIST + 0x28) = 1;
    M32(BLOCK_INDEX) = BLOCK;
    M32(BLOCK) = 0x200;
    M32(STATE) = LIST;
    M32(BASE_VALUE) = CLASS;
    M32(BASE_VALUE + 4) = BASE_ID;
    M32(BASE_ID + 0x10) = BASE_ID;
    M32(BASE_ID + 0x14) = ID_TAG;
    M32(BASE_ID + 0x18) = 2;
    M32(ARG_ID + 0x10) = ARG_ID;
    M32(ARG_ID + 0x14) = ID_TAG;
    M32(ARG_ID + 0x18) = 2;
    M32(ACTOR + 0x0c) = expected_actor_id;
    M8(ACTOR + 0x12) = CLASSIC_CHARACTER_FORM;
}

static void check_call(enum Scenario mode, unsigned expect_reads) {
    scenario = mode;
    live_epoch = expected_epoch;
    field_reads = native_calls = destroys = argument_resolved = 0;
    argument_result = ACTOR;
    if (mode == NO_ARGUMENT || mode == PRIMITIVE || mode == NULL_ID || mode == WRONG_POLICY_TYPE)
        argument_result = 0;
    if (mode == WRONG_ADDRESS) argument_result = OTHER;
    const uint32_t stack = STACK + 0xf00;
    const uint32_t ret = 0x401020;
    protect(ACTORS, PROT_READ | PROT_WRITE);
    M8(ACTOR + 0x12) = mode == WRONG_ACTOR ? CLASSIC_REFERENCE_FORM : CLASSIC_CHARACTER_FORM;
    protect(ACTORS, PROT_READ);
    M32(STATE + 0x1c) = mode == NO_ARGUMENT ? 0 : 1;
    M32(ARG_VALUE) = mode == PRIMITIVE ? 3 : CLASS;
    M32(ARG_VALUE + 4) = mode == NULL_ID ? 0 : ARG_ID;
    M32(SCRIPT_STACK + 0x3c) = M32(SCRIPT_STACK + 0x40) = 0;
    M32(stack) = ret;
    M32(stack + 4) = STACK_REF;
    M32(stack + 8) = 0;
    M32(stack + 12) = REGISTRY + (mode == WRONG_REGISTRY ? 4 : 0);
    M32(stack + 16) = 0;
    CPU cpu = {.fs_base = FS};
    cpu.r[1] = FUNCTION;
    cpu.r[3] = 0xaabb;
    cpu.r[4] = stack;
    cpu.r[6] = 0xccdd;
    cpu.r[7] = 0xeeff;
    ActorScope owner = {.cpu = &cpu, .epoch = live_epoch, .owns_live_reference = mode != NO_REFERENCE};
    active = &owner;
    actor_in_use = true;
    CHECK(M32(M32(FUNCTION) + 15 * 4) == INVOKE);
    f_C46CB0(&cpu);
    actor_in_use = false;
    active = NULL;
    protect(ACTORS, PROT_NONE);
    CHECK(cpu.r[0] == 0);
    CHECK(cpu.r[4] == stack + 20 && M32(stack) == ret);
    CHECK(cpu.r[3] == 0xaabb && cpu.r[6] == 0xccdd && cpu.r[7] == 0xeeff);
    CHECK(M32(TLS + 0x4ac) == 7 && M32(BASE_ID + 0x18) == 2 && M32(ARG_ID + 0x18) == 2);
    CHECK(native_calls == 1 && destroys == 3 && field_reads == expect_reads);
    CHECK(M32(SCRIPT_STACK + 0x3c) == 3);
    CHECK(M32(SCRIPT_STACK + 0x40) == (argument_result ? MATCH_RESULT : EMPTY_RESULT));
    ActorFields out = {0};
    if (owner.epoch == live_epoch) out = owner.copied;
    if (mode == MATCHED) CHECK(out.id == expected_actor_id && out.type == CLASSIC_CHARACTER_FORM);
    else CHECK(!out.id && !out.type);
}

#if !defined(SKYRIM_TEST_PROJECT_FORM)
int main(void) {
    set_up();
    check_call(MATCHED, 1);
    check_call(NO_ARGUMENT, 0);
    check_call(PRIMITIVE, 0);
    check_call(NULL_ID, 0);
    check_call(WRONG_POLICY_TYPE, 0);
    check_call(WRONG_ACTOR, 1);
    check_call(WRONG_ADDRESS, 0);
    check_call(NO_REFERENCE, 0);
    check_call(WRONG_REGISTRY, 0);
    check_call(RETIRED, 1);
    CHECK(!munmap(MEM, GUEST_BYTES));
    puts("PASS stock Invoke and Run, typed Actor argument, and copied owned fields");
    return 0;
}
#else
/* Pass only the copied identity and the already verified fixture VM epoch.
 * The managed argument uses its own authored memory and reference scope. */
void classic_test_managed_after_projection(uint32_t id, uint8_t type, unsigned epoch) {
    CHECK(id && type == CLASSIC_CHARACTER_FORM && epoch);
    expected_actor_id = id;
    expected_epoch = epoch;
    set_up();
    check_call(MATCHED, 1);
    check_call(RETIRED, 1);
    CHECK(!munmap(MEM, GUEST_BYTES));
    puts("PASS scoped managed Actor matches the actor from the accepted companion load");
}
#endif
