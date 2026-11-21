/* SPDX-License-Identifier: GPL-3.0-only */
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

enum {
    RELEASE_IMPORT = 0x0106B154,
    ACTOR = 0x2000, ARMS = 0x3000, BODY = 0x4000, STACK = 0x5000,
    VTABLE = 0x6000, RELEASE = 1, DELETE = 2
};

uint8_t *MEM;
static unsigned releases, deletes;

int classic_camera_visibility_block(CPU *cpu);
void classic_camera_release_block(CPU *cpu);

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    assert(M32(ACTOR + 0x58C) == ARMS); // Original slot clears after release and disposal.
    switch (target) {
    case RELEASE:
        assert(M32(cpu->r[4] + 4) == ARMS + 4 && M32(ARMS + 4) != 0);
        cpu->r[0] = --M32(ARMS + 4);
        cpu->r[4] += 8;
        ++releases;
        break;
    case DELETE:
        assert(cpu->r[1] == ARMS && M32(ARMS + 4) == 0 && releases == 1);
        cpu->r[4] += 4;
        ++deletes;
        break;
    default:
        assert(false);
    }
}

static void check_visibility(unsigned first_person, unsigned arms_culled, unsigned body_culled, unsigned old_mode) {
    CPU cpu = {0};
    cpu.r[2] = first_person;
    cpu.r[3] = ARMS;
    cpu.r[4] = STACK;
    cpu.r[5] = ACTOR;
    cpu.r[6] = BODY;
    M32(ARMS + 0x98) = 0x1000 | arms_culled;
    M32(BODY + 0x98) = 0x2000 | body_culled;
    M8(ACTOR + 0x727) = 0xA0 | old_mode;
    M32(ARMS + 4) = 2;
    M32(BODY + 4) = 3;
    M8(STACK - 4 + 0x13) = 0xFF;

    const bool changes = arms_culled != !first_person || body_culled != first_person;
    assert(classic_camera_visibility_block(&cpu) == changes);
    assert(M32(ARMS + 0x98) == (0x1000u | !first_person));
    assert(M32(BODY + 0x98) == (0x2000u | first_person));
    assert(M8(ACTOR + 0x727) == (0xA0u | (changes ? !first_person : old_mode)));
    assert(M8(STACK - 4 + 0x13) == (changes ? old_mode == first_person : 0xFF));
    assert(M32(ARMS + 4) == 2 && M32(BODY + 4) == 3);
}

static void check_release(unsigned original_refs) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    cpu.r[6] = ACTOR;
    M32(ACTOR + 0x58C) = original_refs ? ARMS : 0;
    M32(ARMS) = VTABLE;
    M32(ARMS + 4) = original_refs;
    M32(VTABLE + 4) = DELETE;
    M32(RELEASE_IMPORT) = RELEASE;
    releases = deletes = 0;

    classic_camera_release_block(&cpu);
    assert(cpu.r[4] == STACK && M32(ACTOR + 0x58C) == 0);
    assert(releases == (original_refs > 0) && deletes == (original_refs == 1));
    assert(M32(ARMS + 4) == (original_refs ? original_refs - 1 : 0));
}

int main(void) {
    MEM = calloc(1, RELEASE_IMPORT + 4);
    assert(MEM);
    for (unsigned mask = 0; mask < 16; ++mask)
        check_visibility(mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
    for (unsigned refs = 0; refs <= 2; ++refs)
        check_release(refs);
    free(MEM);
    return 0;
}
