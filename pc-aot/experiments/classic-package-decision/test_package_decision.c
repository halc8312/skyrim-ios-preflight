/* SPDX-License-Identifier: GPL-3.0-only */
#include "package_decision.h"
#include "form_view.h"
#include "rt_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

enum {
    FORMS = 0x10000,
    STACK = 0x20000,
    ACTOR = FORMS + 0x100,
    WRONG_ACTOR = FORMS + 0x200,
    ORIGINAL = FORMS + 0x300,
    REPLACEMENT = FORMS + 0x400,
    VTABLE = FORMS + 0x500,
    CODE = 0x6a9e90,
    GUEST_BYTES = 0x700000,
    GUEST_RETURN = 0x6ab100,
    ACTOR_ID = 0x1234567,
    ORIGINAL_ID = 0x2345678,
    REPLACEMENT_ID = 0x3456789,
};

uint8_t *MEM;
static bool subscribed, skip_original;
static uint32_t engine_result, resolved_address;
static unsigned original_calls, reads, chooses, lookups;
static ClassicPackageDecision observed;
static ClassicPackageReplacement choice;

void f_6A9E60(CPU *);

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    CHECK(cache && !*cache && target == CODE);
    CHECK(M32(cpu->r[4]) == 0x6a9e7a && M32(cpu->r[4] + 4) == 1);
    cpu->r[0] = skip_original;
    cpu->r[4] += 8;
}

void f_40E4F0(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x6a9e95);
    CHECK(M32(cpu->r[4] + 4) == cpu->r[6] && cpu->r[1] == cpu->r[6] + 0x48);
    ++original_calls;
    cpu->r[0] = engine_result;
    cpu->r[1] = 0x76543210;
    cpu->r[2] = 0x89abcdef;
    cpu->r[4] += 8;
}

bool classic_package_decision_subscribed(void) {
    return subscribed;
}

ClassicPackageReplacement classic_package_decision_choose(ClassicPackageDecision decision) {
    CHECK(original_calls == 1);
    ++chooses;
    observed = decision;
    return choice;
}

bool classic_engine_read_data(const CPU *cpu, uint32_t address, uint8_t *bytes, size_t count) {
    CHECK(cpu && original_calls == 1 && cpu->r[0] == engine_result);
    ++reads;
    if (address < FORMS || (uint64_t)address + count > FORMS + 0x1000) return false;
    memcpy(bytes, MEM + address, count);
    return true;
}

uint32_t classic_engine_lookup_form(const CPU *cpu, uint32_t form_id) {
    CHECK(cpu && original_calls == 1 && cpu->r[0] == engine_result);
    CHECK(chooses == 1 && form_id == choice.package_id);
    ++lookups;
    return resolved_address;
}

static uint32_t run(uint32_t actor, uint32_t original) {
    CPU cpu = {0};
    const uint32_t entry_stack = STACK + 0x800;
    M32(entry_stack) = GUEST_RETURN;
    cpu.r[1] = actor;
    cpu.r[3] = 0x3333;
    cpu.r[4] = entry_stack;
    cpu.r[6] = 0x6666;
    cpu.r[7] = 0x7777;
    observed = (ClassicPackageDecision){0};
    original_calls = reads = chooses = lookups = 0;
    engine_result = original;
    f_6A9E60(&cpu);
    CHECK(cpu.r[4] == entry_stack + 4 && M32(entry_stack) == GUEST_RETURN);
    CHECK(cpu.r[3] == 0x3333 && cpu.r[6] == 0x6666 && cpu.r[7] == 0x7777);
    CHECK(original_calls == !skip_original);
    if (!skip_original) CHECK(cpu.r[1] == 0x76543210 && cpu.r[2] == 0x89abcdef);
    return cpu.r[0];
}

static void form(uint32_t address, uint32_t id, uint8_t type) {
    M32(address + 0x0c) = id;
    M8(address + 0x12) = type;
}

int main(void) {
    const long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size >= 0x1000 && FORMS % page_size == 0 && STACK % page_size == 0);
    MEM = mmap(NULL, GUEST_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(MEM != MAP_FAILED);
    CHECK(!mprotect(MEM + FORMS, page_size, PROT_READ | PROT_WRITE));
    CHECK(!mprotect(MEM + STACK, page_size, PROT_READ | PROT_WRITE));
    CHECK(!mprotect(MEM + (CODE / page_size) * page_size, page_size, PROT_READ));
    M32(ACTOR) = M32(WRONG_ACTOR) = VTABLE;
    M32(VTABLE + 0x264) = CODE;
    form(ACTOR, ACTOR_ID, 0x3e);
    form(WRONG_ACTOR, ACTOR_ID, 0x3d);
    form(ORIGINAL, ORIGINAL_ID, 0x4f);
    form(REPLACEMENT, REPLACEMENT_ID, 0x4f);
    CHECK(!mprotect(MEM + FORMS, page_size, PROT_READ));

    choice = (ClassicPackageReplacement){true, REPLACEMENT_ID};
    resolved_address = REPLACEMENT;
    CHECK(run(ACTOR, ORIGINAL) == ORIGINAL && !reads && !chooses && !lookups);

    subscribed = true;
    skip_original = true;
    CHECK(run(ACTOR, ORIGINAL) == 0 && !reads && !chooses && !lookups);
    skip_original = false;
    CHECK(run(ACTOR, ORIGINAL) == REPLACEMENT && reads == 3 && chooses == 1 && lookups == 1);
    CHECK(observed.actor_id == ACTOR_ID && observed.original_package_id == ORIGINAL_ID);
    CHECK(run(ACTOR, 0) == REPLACEMENT && reads == 2 && chooses == 1 && lookups == 1);
    CHECK(observed.actor_id == ACTOR_ID && observed.original_package_id == 0);

    choice.present = false;
    CHECK(run(ACTOR, ORIGINAL) == ORIGINAL && reads == 2 && chooses == 1 && !lookups);
    choice = (ClassicPackageReplacement){true, 0};
    CHECK(run(ACTOR, ORIGINAL) == ORIGINAL && reads == 2 && chooses == 1 && !lookups);
    choice.package_id = REPLACEMENT_ID;
    CHECK(run(WRONG_ACTOR, ORIGINAL) == ORIGINAL && reads == 1 && !chooses && !lookups);
    CHECK(run(ACTOR, ACTOR) == ACTOR && reads == 2 && !chooses && !lookups);
    CHECK(run(ACTOR, UINT32_MAX - 3) == UINT32_MAX - 3 && reads == 1 && !chooses && !lookups);

    resolved_address = 0;
    CHECK(run(ACTOR, ORIGINAL) == ORIGINAL && reads == 2 && chooses == 1 && lookups == 1);
    resolved_address = ACTOR;
    choice.package_id = ACTOR_ID;
    CHECK(run(ACTOR, ORIGINAL) == ORIGINAL && reads == 3 && chooses == 1 && lookups == 1);
    choice.package_id = REPLACEMENT_ID;
    resolved_address = ORIGINAL;
    CHECK(run(ACTOR, ORIGINAL) == ORIGINAL && reads == 3 && chooses == 1 && lookups == 1);
    resolved_address = CODE;
    CHECK(run(ACTOR, ORIGINAL) == ORIGINAL && reads == 3 && chooses == 1 && lookups == 1);
    CHECK(M8(CODE) == 0 && M32(ACTOR + 0x0c) == ACTOR_ID && M32(ORIGINAL + 0x0c) == ORIGINAL_ID);

    CHECK(!munmap(MEM, GUEST_BYTES));
    puts("PASS hosted package decision, copied IDs, typed replacement, unchanged fallback and read-only code");
    return 0;
}
