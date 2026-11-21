/* SPDX-License-Identifier: GPL-3.0-only */
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
    GUEST_BYTES = 0x1bc0000, DATA = 0x10000, STACK = 0x20000, ACTOR = 0x30100,
    TLS = DATA + 0x100, TLS_SLOTS = DATA + 0x200, PICK = DATA + 0x800,
    CONTROLLER = DATA + 0xa00, HANDLE_VTABLE = DATA + 0xc00, WITHDRAW = DATA + 0xc08,
    OUTER_SP = STACK + 0xf00, WRITER_SP = STACK + 0xc00,
    FORM_ID = 0x1234567, INDEX = 5, GENERATION = 3u << 20, HANDLE = GENERATION | INDEX,
    COUNT_MASK = 0x3ff, HAS_HANDLE = 0x400, OCCUPIED = 1u << 26,
    HANDLE_META = 0x1310638 + INDEX * 8, HANDLE_OBJECT = HANDLE_META + 4,
    FREE_HEAD = 0x131050c, FREE_TAIL = 0x1310510, INVALID_HANDLE = 0x1310630,
    TABLE_GUARD = 0x1b10638, META_COUNT = ACTOR + 0x18,
    PICK_ROOT = 0x1b115d8, TLS_INDEX = 0x1bbeb54, UI_ROOT = 0x12e7454,
    COMPARE_EXCHANGE = 0x106b14c, INCREMENT = 0x106b150, DECREMENT = 0x106b154,
    THREAD_ID = 0x106b20c, DELETE_THIS = 0x400010,
};

enum Scenario {
    MATCHED, NO_OBSERVER, ZERO_HANDLE, STALE_HANDLE, WRONG_INDEX, INACTIVE_SLOT,
    WRONG_TYPE, WRONG_ID, RETIRED_EPOCH, TABLE_WITHDRAWN,
};

typedef struct { uint32_t id; uint8_t type; } ActorCopy;
typedef struct {
    const CPU *caller;
    unsigned epoch;
    ActorCopy copied;
} Scope;

uint8_t *MEM;
static size_t page_size;
static Scope *scope;
static const CPU *writer_cpu;
static enum Scenario scenario;
static unsigned current_epoch, retains, rejects, withdrawals, releases, deletions, reads, ui_calls;

void f_739FD0(CPU *);
void f_43C400(CPU *);

static void protect(uint32_t address, int mode) {
    CHECK(!mprotect(MEM + address / page_size * page_size, page_size, mode));
}

static void withdraw_table_reference(void) {
    CPU cpu = {0};
    cpu.r[4] = WRITER_SP;
    M32(WRITER_SP) = 0x401020;
    M32(WRITER_SP + 4) = WITHDRAW;
    M32(WITHDRAW) = HANDLE;
    writer_cpu = &cpu;
    f_43C400(&cpu);
    writer_cpu = NULL;
    CHECK(cpu.r[4] == WRITER_SP + 4 && M32(WRITER_SP) == 0x401020);
    CHECK(M32(WITHDRAW) == HANDLE && !M32(HANDLE_OBJECT) && !(M32(HANDLE_META) & OCCUPIED));
    CHECK(M32(FREE_HEAD) == INDEX && M32(FREE_TAIL) == INDEX);
    CHECK(M32(META_COUNT) == 1 && !M32(TABLE_GUARD) && !M32(TABLE_GUARD + 4));
}

bool classic_engine_read_data(const CPU *cpu, uint32_t address, uint8_t *bytes, size_t count) {
    CHECK(scope && scope->caller == cpu && (M32(META_COUNT) & COUNT_MASK));
    CHECK(retains == 1 && !rejects && !releases && !deletions && !M32(TABLE_GUARD));
    CHECK(address == ACTOR + 0x0c && count == 7);
    ++reads;
    memcpy(bytes, MEM + address, count);
    return true;
}

void classic_test_retained_actor(const CPU *cpu, uint8_t ok, uint32_t handle, uint32_t out) {
    CHECK(scope && scope->caller == cpu && handle == OUTER_SP - 0x20 && out == OUTER_SP - 0x18);
    CHECK((ok != 0) == (M32(out) != 0));
    if (!ok || scenario == NO_OBSERVER) return;
    CHECK(M32(handle) == HANDLE && M32(out) == ACTOR && retains == 1 && !rejects);
    if (scope->epoch != current_epoch) return;
    if (scenario == TABLE_WITHDRAWN) withdraw_table_reference();
    uint32_t id;
    if (classic_read_form_id(cpu, M32(out), CLASSIC_CHARACTER_FORM, &id) && id == FORM_ID)
        scope->copied = (ActorCopy){id, CLASSIC_CHARACTER_FORM};
}

/* The imported Win32 operations and the final destructor are authored. The
 * separately staged original bodies decide when to acquire and release. */
void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    CHECK(cache && !*cache);
    const uint32_t sp = cpu->r[4];
    const uint32_t ret = M32(sp);
    const bool writer = cpu == writer_cpu;
    uint32_t word;
    switch (target) {
    case THREAD_ID:
        CHECK(writer && ret == 0xa4af5b);
        cpu->r[0] = 1;
        cpu->r[4] += 4;
        return;
    case COMPARE_EXCHANGE:
        CHECK(writer && M32(sp + 4) == TABLE_GUARD);
        if (ret == 0xa4af84) {
            CHECK(!M32(TABLE_GUARD) && M32(sp + 8) == 0x80000001 && !M32(sp + 12));
        } else {
            CHECK(ret == 0xa4b11e && M32(TABLE_GUARD) == 0x80000001);
            CHECK(!M32(sp + 8) && M32(sp + 12) == 0x80000001);
        }
        word = M32(sp + 12);
        CHECK(__atomic_compare_exchange_n((uint32_t *)(MEM + TABLE_GUARD), &word,
                                          M32(sp + 8), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        cpu->r[0] = word;
        cpu->r[4] += 16;
        return;
    case INCREMENT:
        CHECK(!writer && ret == 0x4a81f1 && M32(sp + 4) == META_COUNT);
        CHECK(!retains && !releases && (M32(META_COUNT) & COUNT_MASK) == 1);
        ++retains;
        cpu->r[0] = __atomic_add_fetch((uint32_t *)(MEM + META_COUNT), 1u, __ATOMIC_SEQ_CST);
        cpu->r[4] += 8;
        return;
    case DECREMENT:
        CHECK(M32(sp + 4) == META_COUNT && retains == 1);
        if (writer) {
            CHECK(ret == 0x43c474 && M32(TABLE_GUARD) == 0x80000001 && M32(META_COUNT) == 2);
            ++withdrawals;
        } else if (ret == 0x4a91fd) {
            CHECK(!reads && !ui_calls && !releases);
            ++rejects;
        } else {
            CHECK(ret == 0x73a234 && ui_calls == 1 && !rejects && !releases);
            ++releases;
        }
        cpu->r[0] = __atomic_sub_fetch((uint32_t *)(MEM + META_COUNT), 1u, __ATOMIC_SEQ_CST);
        cpu->r[4] += 8;
        return;
    case DELETE_THIS:
        CHECK(!writer && ret == 0x73a244 && cpu->r[1] == ACTOR + 0x14 && !M32(META_COUNT));
        CHECK(releases == 1 && withdrawals == 1 && !deletions);
        ++deletions;
        protect(ACTOR, PROT_NONE);
        cpu->r[4] += 4;
        return;
    default:
        CHECK(false);
    }
}

void f_771200(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x73a075 && retains == 1 && !rejects && !releases);
    if (scenario == RETIRED_EPOCH) ++current_epoch;
    cpu->r[0] = 0;
    cpu->r[4] += 4;
}

void f_85FBD0(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x73a220 && !ui_calls);
    ++ui_calls;
    cpu->r[0] = 0x7a;
    cpu->r[4] += 4;
}

#define UNREACHED(name) void name(CPU *cpu) { (void)cpu; CHECK(false); }
UNREACHED(f_402B20)
UNREACHED(f_402BE0)
UNREACHED(f_402C00)
UNREACHED(f_40A760)
UNREACHED(f_431B00)
UNREACHED(f_4DE820)
UNREACHED(f_515DE0)
UNREACHED(f_54B400)
UNREACHED(f_A5C300)

static uint32_t check_call(enum Scenario mode, bool matched, unsigned expect_reads, bool expect_output) {
    scenario = mode;
    retains = rejects = withdrawals = releases = deletions = reads = ui_calls = 0;
    protect(ACTOR, PROT_READ | PROT_WRITE);
    M32(ACTOR + 0x0c) = FORM_ID + (mode == WRONG_ID);
    M8(ACTOR + 0x12) = mode == WRONG_TYPE ? CLASSIC_REFERENCE_FORM : CLASSIC_CHARACTER_FORM;
    M32(ACTOR + 0x14) = HANDLE_VTABLE;
    M32(META_COUNT) = ((INDEX + (mode == WRONG_INDEX)) << 11) | HAS_HANDLE | 1;
    M32(HANDLE_META) = GENERATION | (mode == INACTIVE_SLOT ? 0 : OCCUPIED);
    M32(HANDLE_OBJECT) = ACTOR + 0x14;
    M32(FREE_HEAD) = M32(FREE_TAIL) = UINT32_MAX;
    uint32_t selected = mode == ZERO_HANDLE ? 0 : HANDLE;
    if (mode == STALE_HANDLE) selected ^= 1u << 20;
    M32(PICK + 4) = selected;
    M32(OUTER_SP) = 0x401000;
    CPU cpu = {.fs_base = DATA};
    cpu.r[1] = CONTROLLER;
    cpu.r[3] = 0xaabb;
    cpu.r[4] = OUTER_SP;
    cpu.r[5] = 0xbbcc;
    cpu.r[6] = 0xccdd;
    cpu.r[7] = 0xddee;
    Scope call = {.caller = &cpu, .epoch = current_epoch};
    scope = &call;
    f_739FD0(&cpu);
    scope = NULL;
    CHECK(cpu.r[4] == OUTER_SP + 4 && M32(OUTER_SP) == 0x401000);
    CHECK(cpu.r[3] == 0xaabb && cpu.r[5] == 0xbbcc && cpu.r[6] == 0xccdd && cpu.r[7] == 0xddee);
    CHECK(M32(TLS + 0x4ac) == 7 && M32(PICK + 4) == selected && ui_calls == 1);
    CHECK(retains == (mode != ZERO_HANDLE) && rejects == (!matched && mode != ZERO_HANDLE));
    CHECK(releases == matched && reads == expect_reads);
    CHECK(withdrawals == (mode == TABLE_WITHDRAWN) && deletions == withdrawals);
    if (!deletions) {
        CHECK((M32(META_COUNT) & COUNT_MASK) == 1);
        protect(ACTOR, PROT_NONE);
    }
    if (call.epoch != current_epoch) call.copied = (ActorCopy){0};
    CHECK(call.copied.id == (expect_output ? FORM_ID : 0));
    CHECK(call.copied.type == (expect_output ? CLASSIC_CHARACTER_FORM : 0));
    return cpu.r[0];
}

int main(void) {
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page_size >= 4096 && GUEST_BYTES % page_size == 0);
    MEM = mmap(NULL, GUEST_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(MEM != MAP_FAILED);
    const uint32_t pages[] = {DATA, STACK, HANDLE_META, TABLE_GUARD, PICK_ROOT, UI_ROOT, TLS_INDEX,
                              COMPARE_EXCHANGE};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) protect(pages[i], PROT_READ | PROT_WRITE);
    M32(DATA + 0x2c) = TLS_SLOTS;
    M32(TLS_SLOTS) = TLS;
    M32(TLS + 0x4ac) = 7;
    M32(TLS_INDEX) = 0;
    M32(PICK_ROOT) = PICK;
    M32(INVALID_HANDLE) = 0;
    M32(CONTROLLER + 0x138) = 2;
    M32(HANDLE_VTABLE + 4) = DELETE_THIS;
    M32(COMPARE_EXCHANGE) = COMPARE_EXCHANGE;
    M32(INCREMENT) = INCREMENT;
    M32(DECREMENT) = DECREMENT;
    M32(THREAD_ID) = THREAD_ID;
    protect(COMPARE_EXCHANGE, PROT_READ);
    current_epoch = 29;
    uint32_t stock = check_call(NO_OBSERVER, true, 0, false);
    CHECK(check_call(MATCHED, true, 1, true) == stock);
    check_call(ZERO_HANDLE, false, 0, false);
    check_call(STALE_HANDLE, false, 0, false);
    check_call(WRONG_INDEX, false, 0, false);
    check_call(INACTIVE_SLOT, false, 0, false);
    check_call(WRONG_TYPE, true, 1, false);
    check_call(WRONG_ID, true, 1, false);
    check_call(RETIRED_EPOCH, true, 1, false);
    check_call(TABLE_WITHDRAWN, true, 1, true);
    CHECK(!munmap(MEM, GUEST_BYTES));
    puts("PASS original reference retain, table withdrawal, copied Actor and final release");
    return 0;
}
