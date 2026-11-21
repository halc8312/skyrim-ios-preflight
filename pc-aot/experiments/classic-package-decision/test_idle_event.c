/* SPDX-License-Identifier: GPL-3.0-only */
#include "form_view.h"
#include "idle_event.h"
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
    REQUESTS = 0x30000,
    ACTOR = FORMS + 0x100,
    REFERENCE = FORMS + 0x200,
    OLD_IDLE = FORMS + 0x300,
    NEW_IDLE = FORMS + 0x400,
    OLD_EVENT = FORMS + 0x500,
    NEW_EVENT = FORMS + 0x600,
    LONG_EVENT = FORMS + 0x700,
    REQUEST = REQUESTS + 0x100,
    CODE = 0x6f02ff,
    RECEIVER = 0x800,
    GUEST_BYTES = 0x700000,
    TES_ACTION_DATA = 0x107f494,
    ACTOR_ID = 0x123456,
    REFERENCE_ID = 0x234567,
    OLD_IDLE_ID = 0x345678,
    NEW_IDLE_ID = 0x456789,
};

uint8_t *MEM;
static bool subscribed, owned = true, borrow_active, preparation_ok = true, fail_apply;
static uint32_t object_lookup, idle_lookup;
static unsigned prepared, finished, borrows, releases, choices, applies, retains, drops;
static ClassicIdleEvent delivered;
static ClassicIdleReplacement choice = {true, NEW_IDLE_ID};

void f_6F02D0(CPU *);
void f_A51280(CPU *);

void f_64F710(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x6f02e7 && cpu->r[1] == M32(REQUEST + 4) + 0x20);
    cpu->r[0] = 1;
    cpu->r[4] += 4;
}

void f_6F01B0(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x6f02f9 && M32(cpu->r[4] + 4) == REQUEST);
    CHECK(!borrows && cpu->r[1] == RECEIVER);
    ++prepared;
    cpu->r[0] = preparation_ok ? 0x551001 : 0x551000;
    cpu->r[4] += 8;
}

void f_6F0210(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x6f031a && M32(cpu->r[4] + 4) == REQUEST);
    CHECK(!(M8(REQUEST + 0x2c) & 1) && !borrow_active && cpu->r[1] == RECEIVER);
    ++finished;
    cpu->r[0] = 0x89abc055;
    cpu->r[4] += 8;
}

void f_6F1690(CPU *cpu) {
    (void)cpu;
    CHECK(false); /* The fixture does not enter the unsuccessful cleanup case. */
}

void f_A50A30(CPU *cpu) {
    CHECK(borrow_active && M32(cpu->r[4]) == 0xa512a1);
    CHECK(M32(cpu->r[4] + 4) == NEW_EVENT - 12 && M32(REQUEST + 0x14) == OLD_EVENT);
    CHECK(!retains && !drops);
    ++retains;
    cpu->r[4] += 4;
}

void f_A51C20(CPU *cpu) {
    CHECK(borrow_active && M32(cpu->r[4]) == 0xa512b0);
    const uint32_t old_ref = M32(cpu->r[4] + 4);
    CHECK(old_ref == STACK + 0x304 && M32(old_ref) == OLD_EVENT);
    CHECK(M32(REQUEST + 0x14) == NEW_EVENT);
    CHECK(retains == 1 && !drops);
    ++drops;
    cpu->r[4] += 4;
}

bool classic_idle_event_subscribed(void) { return subscribed; }

ClassicIdleReplacement classic_idle_event_choose(ClassicIdleEvent event) {
    CHECK(borrow_active && !applies && !finished);
    ++choices;
    delivered = event;
    return choice;
}

bool classic_engine_idle_begin(const CPU *cpu, uint32_t request) {
    CHECK(!borrow_active && prepared == 1 && !finished && request == REQUEST);
    CHECK(cpu->r[6] == request && (uint8_t)cpu->r[0] == 1);
    ++borrows;
    borrow_active = owned;
    return owned;
}

void classic_engine_idle_end(const CPU *cpu, uint32_t request) {
    CHECK(borrow_active && request == REQUEST && cpu->r[6] == REQUEST);
    CHECK(!finished && borrows == 1 && !releases);
    ++releases;
    borrow_active = false;
}

bool classic_engine_read_data(const CPU *cpu, uint32_t address, uint8_t *bytes, size_t count) {
    CHECK(borrow_active && cpu->r[6] == REQUEST);
    if (address == REQUEST) {
        if (count > 0x30) return false;
    } else if (address < FORMS || (uint64_t)address + count > FORMS + 0x1000) {
        return false;
    }
    memcpy(bytes, MEM + address, count);
    return true;
}

uint32_t classic_engine_lookup_form(const CPU *cpu, uint32_t id) {
    CHECK(borrow_active && cpu->r[6] == REQUEST);
    if (id == ACTOR_ID || id == REFERENCE_ID) return object_lookup;
    CHECK(id == choice.idle_id);
    return idle_lookup;
}

bool classic_engine_idle_apply(const CPU *cpu, uint32_t request, uint32_t idle) {
    CHECK(borrow_active && request == REQUEST && idle == NEW_IDLE && choices == 1);
    ++applies;
    if (fail_apply) return false;

    CHECK(M32(request + 0x14) == OLD_EVENT && M32(request + 0x24) == OLD_IDLE);
    CPU copy = *cpu;
    copy.r[4] = STACK + 0x300;
    copy.r[1] = request + 0x14;
    M32(copy.r[4]) = 0x6f02ff;
    M32(copy.r[4] + 4) = idle + 0x30;
    f_A51280(&copy);
    CHECK(copy.r[4] == STACK + 0x308 && M32(request + 0x14) == NEW_EVENT);
    M32(request + 0x24) = idle;
    return true;
}

static void run(uint32_t vtable, uint32_t object, uint32_t event, bool bit, bool replaced) {
    CPU cpu = {0};
    const uint32_t entry_stack = STACK + 0x800;
    cpu.r[1] = RECEIVER;
    cpu.r[3] = 0x3333;
    cpu.r[4] = entry_stack;
    cpu.r[6] = 0x6666;
    cpu.r[7] = 0x7777;
    M32(entry_stack) = 0x6b644d;
    M32(entry_stack + 4) = REQUEST;
    M32(REQUEST) = vtable;
    M32(REQUEST + 4) = object;
    M32(REQUEST + 0x14) = event;
    M32(REQUEST + 0x18) = 0x12345678;
    M32(REQUEST + 0x24) = OLD_IDLE;
    M32(REQUEST + 0x2c) = bit;
    prepared = finished = borrows = releases = choices = applies = retains = drops = 0;
    delivered = (ClassicIdleEvent){0};

    f_6F02D0(&cpu);

    CHECK(!borrow_active && prepared == 1 && finished == (unsigned)!bit);
    CHECK(M32(REQUEST + 0x2c) == (bit ? (preparation_ok ? 2u : 1u) : 0u));
    CHECK(M32(REQUEST + 0x14) == (replaced ? NEW_EVENT : event));
    CHECK(M32(REQUEST + 0x24) == (replaced ? NEW_IDLE : OLD_IDLE));
    CHECK(M32(REQUEST + 0x18) == 0x12345678 && retains == replaced && drops == replaced);
    CHECK(cpu.r[4] == entry_stack + 8 && M32(entry_stack) == 0x6b644d);
    CHECK(cpu.r[3] == 0x3333 && cpu.r[6] == 0x6666 && cpu.r[7] == 0x7777);
    CHECK(cpu.r[0] == (bit ? (preparation_ok ? 0x551001u : 0x551000u) : 0x89abc055u));
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
    CHECK(!mprotect(MEM + REQUESTS, page_size, PROT_READ | PROT_WRITE));
    CHECK(!mprotect(MEM + (CODE / page_size) * page_size, page_size, PROT_READ));
    form(ACTOR, ACTOR_ID, CLASSIC_CHARACTER_FORM);
    form(REFERENCE, REFERENCE_ID, CLASSIC_REFERENCE_FORM);
    form(OLD_IDLE, OLD_IDLE_ID, CLASSIC_IDLE_FORM);
    form(NEW_IDLE, NEW_IDLE_ID, CLASSIC_IDLE_FORM);
    M32(NEW_IDLE + 0x30) = NEW_EVENT;
    memcpy(MEM + OLD_EVENT, "fixtureOriginal", sizeof("fixtureOriginal"));
    memcpy(MEM + NEW_EVENT, "fixtureOverride", sizeof("fixtureOverride"));
    memset(MEM + LONG_EVENT, 'x', CLASSIC_IDLE_EVENT_BYTES);
    CHECK(!mprotect(MEM + FORMS, page_size, PROT_READ));

    object_lookup = ACTOR;
    idle_lookup = NEW_IDLE;
    run(TES_ACTION_DATA, ACTOR, OLD_EVENT, false, false);
    CHECK(!borrows && !choices);
    subscribed = true;
    preparation_ok = false;
    run(TES_ACTION_DATA, ACTOR, OLD_EVENT, true, false);
    CHECK(!borrows && !choices);
    preparation_ok = true;
    owned = false;
    run(TES_ACTION_DATA, ACTOR, OLD_EVENT, true, false);
    CHECK(borrows == 1 && !releases && !choices);
    owned = true;

    run(TES_ACTION_DATA, ACTOR, OLD_EVENT, false, true);
    CHECK(borrows == 1 && releases == 1 && choices == 1 && applies == 1);
    CHECK(delivered.object_id == ACTOR_ID && !strcmp(delivered.event_name, "fixtureOriginal"));
    object_lookup = REFERENCE;
    run(TES_ACTION_DATA, REFERENCE, OLD_EVENT, true, true);
    CHECK(releases == 1 && applies == 1 && delivered.object_id == REFERENCE_ID);
    object_lookup = ACTOR;

    choice.present = false;
    run(TES_ACTION_DATA, ACTOR, OLD_EVENT, false, false);
    CHECK(choices == 1 && !applies && releases == 1);
    choice.present = true;
    fail_apply = true;
    run(TES_ACTION_DATA, ACTOR, OLD_EVENT, true, false);
    CHECK(choices == 1 && applies == 1 && releases == 1);
    fail_apply = false;

    run(TES_ACTION_DATA, ACTOR, LONG_EVENT, false, false);
    CHECK(releases == 1 && !choices);
    run(TES_ACTION_DATA, ACTOR, 0, false, false);
    CHECK(releases == 1 && !choices);
    run(TES_ACTION_DATA, ACTOR, CODE, false, false);
    CHECK(releases == 1 && !choices);
    run(0, ACTOR, OLD_EVENT, false, false);
    CHECK(releases == 1 && !choices);
    object_lookup = 0;
    run(TES_ACTION_DATA, ACTOR, OLD_EVENT, false, false);
    CHECK(releases == 1 && !choices);
    object_lookup = ACTOR;

    const uint32_t rejected_idle[] = {0, ACTOR, OLD_IDLE, CODE & ~3u};
    for (size_t i = 0; i < sizeof(rejected_idle) / sizeof(rejected_idle[0]); ++i) {
        idle_lookup = rejected_idle[i];
        run(TES_ACTION_DATA, ACTOR, OLD_EVENT, false, false);
        CHECK(releases == 1 && choices == 1 && !applies);
    }
    classic_idle_event_before_test(&(CPU){0}, UINT32_MAX - 3);
    CHECK(borrows == 1 && M8(CODE) == 0 && M32(NEW_IDLE + 0x30) == NEW_EVENT);

    CHECK(!munmap(MEM, GUEST_BYTES));
    puts("PASS hosted idle replacement, owned request, lifted string references and both original branches");
    return 0;
}
