/* SPDX-License-Identifier: GPL-3.0-only */
#include "form_view.h"
#include "package_change.h"
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
    PACKAGE = FORMS + 0x200,
    OTHER_PACKAGE = FORMS + 0x300,
    ACTOR_COPY = FORMS + 0x400,
    PACKET = STACK + 0x100,
    RECEIVER = 0x800,
    CODE = 0x8cab54,
    GUEST_BYTES = 0x900000,
    PACKAGE_ID = 0x22345678,
    OTHER_ID = 0x22345679,
};

static const uint32_t actor_id = 0xff123456;
static const uint32_t string_refs[] = {0x1b4070c, 0x1b40708, 0x1b40704};
static const uint32_t assignment_returns[] = {0x8cab0b, 0x8cab34, 0x8cab59};
static const uint32_t emission_returns[] = {0x8cab24, 0x8cab4d, 0x8cab72};

uint8_t *MEM;
static bool subscribed, reject_packet;
static uint32_t actor_result, package_result, package_arg;
static unsigned kind, reads, lookups, phase_count;
static enum { NOTIFIED, ASSIGNED, EMITTED } phases[3];
static ClassicPackageChange delivered;

void f_8CA9B0(CPU *);

void f_A50D50(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x8caaf2 && M32(cpu->r[4] + 4) == 0x12e32ec);
    cpu->r[4] += 8;
}

void f_A51280(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == assignment_returns[kind]);
    CHECK(M32(cpu->r[4] + 4) == string_refs[kind] && cpu->r[1] == STACK + 0x810);
    CHECK(phase_count <= 1);
    phases[phase_count++] = ASSIGNED;
    M32(cpu->r[1]) = M32(cpu->r[4] + 4);
    cpu->r[4] += 8;
}

static void emit(CPU *cpu, unsigned phase) {
    CHECK(kind == phase && M32(cpu->r[4]) == emission_returns[phase]);
    CHECK(M32(cpu->r[4] + 4) == package_arg && cpu->r[1] == RECEIVER + 0x228);
    CHECK(phase_count && phase_count < 3 && phases[phase_count - 1] == ASSIGNED);
    phases[phase_count++] = EMITTED;
    cpu->r[0] = 0x55;
    cpu->r[4] += 16;
}

void f_8B03E0(CPU *cpu) { emit(cpu, 0); }
void f_8B04D0(CPU *cpu) { emit(cpu, 1); }
void f_8B05C0(CPU *cpu) { emit(cpu, 2); }

bool classic_package_change_subscribed(void) { return subscribed; }

void classic_package_change_notify(ClassicPackageChange event) {
    CHECK(!phase_count && kind == 2);
    phases[phase_count++] = NOTIFIED;
    delivered = event;
}

bool classic_engine_read_data(const CPU *cpu, uint32_t address, uint8_t *bytes, size_t count) {
    CHECK(M32(cpu->r[4]) == assignment_returns[2] && !phase_count);
    ++reads;
    if (address == PACKET) {
        if (reject_packet || count != 8) return false;
    } else if (address < FORMS || (uint64_t)address + count > FORMS + 0x1000) {
        return false;
    }
    memcpy(bytes, MEM + address, count);
    return true;
}

uint32_t classic_engine_lookup_form(const CPU *cpu, uint32_t id) {
    CHECK(M32(cpu->r[4]) == assignment_returns[2] && !phase_count);
    ++lookups;
    if (id == actor_id) return actor_result;
    CHECK(id == package_arg);
    return package_result;
}

static void run(unsigned event_kind, uint32_t actor, uint32_t package, bool expect_notify) {
    CPU cpu = {0};
    cpu.r[3] = RECEIVER;
    cpu.r[4] = STACK + 0x800;
    cpu.r[6] = 1;
    kind = event_kind;
    package_arg = package;
    M32(cpu.r[4] + 0x3c) = PACKET;
    M32(cpu.r[4] + 0x10) = 0;
    M32(PACKET) = actor;
    M32(PACKET + 4) = package;
    M32(PACKET + 8) = event_kind;
    delivered = (ClassicPackageChange){0};
    phase_count = reads = lookups = 0;
    f_8CA9B0(&cpu);
    CHECK(phase_count == (expect_notify ? 3u : 2u));
    if (expect_notify) {
        CHECK(phases[0] == NOTIFIED && delivered.actor_id == actor_id);
        CHECK(delivered.outgoing_package_id == package);
    } else {
        CHECK(!delivered.actor_id && !delivered.outgoing_package_id);
    }
    CHECK(phases[phase_count - 2] == ASSIGNED && phases[phase_count - 1] == EMITTED);
    CHECK(M32(STACK + 0x810) == string_refs[event_kind]);
    CHECK(cpu.r[0] == 0x55 && cpu.r[4] == STACK + 0x800 && cpu.r[7] == PACKET);
    CHECK(M32(PACKET) == actor && M32(PACKET + 4) == package && M32(PACKET + 8) == event_kind);
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
    form(ACTOR, actor_id, CLASSIC_CHARACTER_FORM);
    form(ACTOR_COPY, actor_id, CLASSIC_CHARACTER_FORM);
    form(PACKAGE, PACKAGE_ID, CLASSIC_PACKAGE_FORM);
    form(OTHER_PACKAGE, OTHER_ID, CLASSIC_PACKAGE_FORM);
    CHECK(!mprotect(MEM + FORMS, page_size, PROT_READ));

    actor_result = ACTOR;
    package_result = PACKAGE;
    run(2, ACTOR, PACKAGE_ID, false);
    CHECK(!reads && !lookups);
    subscribed = true;
    run(0, ACTOR, PACKAGE_ID, false);
    CHECK(!reads && !lookups);
    run(1, ACTOR, PACKAGE_ID, false);
    CHECK(!reads && !lookups);
    run(2, ACTOR, PACKAGE_ID, true);
    CHECK(reads == 3 && lookups == 2);

    reject_packet = true;
    run(2, ACTOR, PACKAGE_ID, false);
    CHECK(reads == 1 && !lookups);
    reject_packet = false;
    run(2, ACTOR, 0, false);
    CHECK(reads == 1 && !lookups);
    run(2, 0, PACKAGE_ID, false);
    CHECK(reads == 1 && !lookups);
    run(2, ACTOR + 1, PACKAGE_ID, false);
    CHECK(reads == 1 && !lookups);
    run(2, PACKAGE, PACKAGE_ID, false);
    CHECK(reads == 2 && !lookups);
    actor_result = ACTOR_COPY;
    run(2, ACTOR, PACKAGE_ID, false);
    CHECK(reads == 2 && lookups == 1);
    actor_result = ACTOR;

    package_result = 0;
    run(2, ACTOR, PACKAGE_ID, false);
    CHECK(reads == 2 && lookups == 2);
    const uint32_t rejected_packages[] = {ACTOR, OTHER_PACKAGE, CODE};
    for (size_t i = 0; i < sizeof(rejected_packages) / sizeof(rejected_packages[0]); ++i) {
        package_result = rejected_packages[i];
        run(2, ACTOR, PACKAGE_ID, false);
        CHECK(reads == 3 && lookups == 2);
    }
    classic_package_change_before_assign(&(CPU){0}, UINT32_MAX - 2);
    CHECK(reads == 3 && lookups == 2);
    CHECK(M8(CODE) == 0 && M32(ACTOR + 0x0c) == actor_id && M32(PACKAGE + 0x0c) == PACKAGE_ID);

    CHECK(!munmap(MEM, GUEST_BYTES));
    puts("PASS hosted OnPackageChange notification, checked actor/package IDs, original assignment/emission order");
    return 0;
}
