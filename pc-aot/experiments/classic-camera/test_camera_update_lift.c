/* SPDX-License-Identifier: GPL-3.0-only */
#include "lift_prelude.h"

#include <assert.h>
#include <stdlib.h>

enum {
    UPDATE_DELTA = 0x012E355C,
    ARMS = 0x3000, BODY = 0x4000, STACK = 0x5000,
    OLD_PARENT = 0x6000, NEW_PARENT = 0x7000, NODE_VTABLE = 0x8000, PARENT_VTABLE = 0x9000,
    FRAME = 0xA000, SCENE_CONTEXT = 0xB000, DOWNWARD = 1, UPDATE_BOUND = 2
};

uint8_t *MEM;
static uint32_t expected_root, expected_context, expected_delta, expected_flags, next_parent;
static unsigned downward, parent_bounds, render_calls;

void classic_camera_body_update(CPU *cpu);
void classic_camera_arms_update(CPU *cpu);
void classic_camera_render_arms(CPU *cpu);

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    switch (target) {
    case DOWNWARD: {
        const uint32_t context = M32(cpu->r[4] + 4);
        assert(++downward == 1 && parent_bounds == 0);
        assert(cpu->r[1] == expected_root && context == expected_context && M32(cpu->r[4] + 8) == 0);
        assert(M32(context) == expected_delta && M32(context + 4) == expected_flags);
        // The authored traversal can attach, remove or replace the parent.
        M32(expected_root + 0x18) = next_parent;
        cpu->r[4] += 12;
        break;
    }
    case UPDATE_BOUND:
        assert(++parent_bounds == 1 && downward == 1);
        assert(cpu->r[1] == next_parent && next_parent != 0);
        cpu->r[4] += 4;
        break;
    default:
        assert(0);
    }
}

void f_C7A510(CPU *cpu) {
    assert(++render_calls == 1);
    assert(cpu->r[1] == SCENE_CONTEXT && M32(cpu->r[4] + 4) == SCENE_CONTEXT);
    assert(M32(ARMS + 0x98) == 0x1000 && M32(BODY + 0x98) == 0x2001);
    M32(ARMS + 0x98) |= 0x4000; // An unrelated nested update must survive the original cull restore.
    cpu->r[4] += 4; // The renderer helper is cdecl and returns with the argument still on stack.
}

static void check_update(void (*update)(CPU *), uint32_t root, uint32_t initial_parent, uint32_t final_parent) {
    CPU cpu = {0};
    cpu.r[0] = cpu.r[3] = root;
    cpu.r[4] = STACK;
    M32(root + 0x18) = initial_parent;
    expected_root = root;
    expected_context = root == ARMS ? STACK + 4 : STACK + 0x2C;
    expected_delta = root == ARMS ? M32(UPDATE_DELTA) : 0;
    expected_flags = root == ARMS ? 0x2001 : 0;
    next_parent = final_parent;
    downward = parent_bounds = 0;
    update(&cpu);
    assert(downward == 1 && parent_bounds == (final_parent != 0));
    assert(M32(root + 0x18) == final_parent && cpu.r[4] == STACK);
    assert(M32(ARMS + 4) == 2 && M32(BODY + 4) == 3);
}

int main(void) {
    MEM = calloc(1, UPDATE_DELTA + 4);
    assert(MEM);
    M32(UPDATE_DELTA) = 0x3C888889;
    M32(ARMS) = M32(BODY) = NODE_VTABLE;
    M32(ARMS + 4) = 2;
    M32(BODY + 4) = 3;
    M32(NODE_VTABLE + 0xA4) = DOWNWARD;
    M32(OLD_PARENT) = M32(NEW_PARENT) = PARENT_VTABLE;
    M32(PARENT_VTABLE + 0xEC) = UPDATE_BOUND;
    check_update(classic_camera_body_update, BODY, OLD_PARENT, NEW_PARENT);
    check_update(classic_camera_body_update, BODY, 0, 0);
    check_update(classic_camera_arms_update, ARMS, OLD_PARENT, 0);
    check_update(classic_camera_arms_update, ARMS, 0, NEW_PARENT);

    CPU cpu = {0};
    cpu.r[4] = STACK;
    cpu.r[6] = FRAME;
    cpu.r[7] = ARMS;
    M32(FRAME + 0xEC) = SCENE_CONTEXT;
    M32(ARMS + 0x98) = 0x1001;
    M32(BODY + 0x98) = 0x2001;
    classic_camera_render_arms(&cpu);
    assert(render_calls == 1 && cpu.r[4] == STACK);
    assert(M32(ARMS + 0x98) == 0x5001 && M32(BODY + 0x98) == 0x2001);
    free(MEM);
    return 0;
}
