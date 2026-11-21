/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "highheel_height.h"
#include "lift_prelude.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STACK = 0x2f00, NODE_TABLE = 0x2100, ROOT = 0x2200, NPC = 0x2300,
    OTHER_ROOT = 0x2400, OTHER_NPC = 0x2500, CHILDREN = 0x2600, OTHER_CHILDREN = 0x2610,
    LEG_CHILDREN = 0x2620, LEG_NAMES = 0x2800, LEG_NODES = 0x2a00, LEG_COUNT = 4,
    INTERNED_NPC = 0x2700, INTERNED_ROOT = 0x2710, NAME = 0x2720, CONTEXT = 0x2730,
    GET_NAME = 0xaae1d0, REFRESH = 0xab53e0,
};

uint8_t *MEM;

void f_AAE1D0(CPU *);
void f_AB53E0(CPU *);

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    assert(target == GET_NAME && M32(cpu->r[4]) == 0xaae208);
    f_AAE1D0(cpu);
}

void rt_call(CPU *cpu, uint32_t target)
{
    (void)cpu;
    (void)target;
    assert(0 && "alternate world refresh is outside the fixture");
}

typedef struct Scene {
    uint32_t root;
    uint64_t generation;
    bool replace_before_commit;
    unsigned refreshes;
} Scene;

static uint32_t find_named_node(const Scene *scene, uint32_t interned_name)
{
    CPU cpu = {.r = {0, scene->root, 0, 0, STACK}};
    M32(NAME) = interned_name;
    M32(STACK + 4) = NAME;
    assert(M32(M32(scene->root) + 0x9c) == GET_NAME);
    f_AAE1D0(&cpu);
    assert(cpu.r[4] == STACK + 8);
    return cpu.r[0];
}

static bool read_node(void *actor, const char *name, ClassicHeelNode *out)
{
    assert(!strcmp(name, "NPC"));
    const Scene *scene = actor;
    uint32_t node = find_named_node(scene, INTERNED_NPC);
    if (!node || M32(node + 0x18) != scene->root)
        return false;
    *out = (ClassicHeelNode){scene->generation, node, MF32(node + 0x4c)};
    return true;
}

static bool commit_node(void *actor, const ClassicHeelNode *observed, float local_z)
{
    Scene *scene = actor;
    if (scene->replace_before_commit) {
        scene->replace_before_commit = false;
        scene->root = OTHER_ROOT;
        ++scene->generation;
    }
    ClassicHeelNode current = {0};
    if (!read_node(scene, "NPC", &current) || current.root_generation != observed->root_generation ||
        current.node_generation != observed->node_generation || current.local_z != observed->local_z) {
        return false;
    }
    /* Only the authored guest scene uses this commit. The selected original
     * body has a separate indirect path when the auxiliary slot is present. */
    uint32_t node = (uint32_t)current.node_generation;
    if (M32(node + 0x1c) || M32(M32(node) + 0xb4) != REFRESH)
        return false;
    MF32(node + 0x4c) = local_z;
    CPU cpu = {.r = {0, node, 0, 0, STACK}};
    M32(STACK + 4) = CONTEXT;
    f_AB53E0(&cpu);
    assert(cpu.r[4] == STACK + 8);
    ++scene->refreshes;
    return true;
}

static void make_scene(uint32_t root, uint32_t node, uint32_t children, float height)
{
    M32(root) = M32(node) = NODE_TABLE;
    M32(root + 8) = INTERNED_ROOT;
    M32(node + 8) = INTERNED_NPC;
    M32(node + 0x18) = root;
    M32(root + 0xac) = children;
    M16(root + 0xb2) = 1;
    M32(children) = node;
    MF32(root + 0x54) = MF32(root + 0x64) = MF32(root + 0x74) = 1.0f;
    MF32(root + 0x78) = 100.0f;
    MF32(root + 0x7c) = 200.0f;
    MF32(root + 0x80) = 300.0f;
    MF32(root + 0x84) = 2.0f;
    MF32(node + 0x20) = MF32(node + 0x30) = MF32(node + 0x40) = 1.0f;
    MF32(node + 0x44) = 1.0f;
    MF32(node + 0x48) = 3.0f;
    MF32(node + 0x4c) = height;
    MF32(node + 0x50) = 1.0f;
}

static void test_pose_on_named_nodes(void)
{
    static const char *const names[LEG_COUNT] = {
        "NPC L Foot [Lft ]", "NPC L Calf [LClf]", "NPC R Foot [Rft ]", "NPC R Calf [RClf]",
    };
    static const ClassicHeelPoint points[LEG_COUNT] = {{15, 0, 0}, {15, 0, 30}, {0, 0, 0}, {24, 0, 18}};
    make_scene(ROOT, NPC, CHILDREN, 10.0f);
    MF32(NPC + 0x84) = 2.0f;
    M32(NPC + 0xac) = LEG_CHILDREN;
    M16(NPC + 0xb2) = LEG_COUNT;
    for (unsigned i = 0; i < LEG_COUNT; ++i) {
        uint32_t node = LEG_NODES + i * 0x100;
        uint32_t name = LEG_NAMES + i * 32;
        strcpy((char *)MEM + name, names[i]);
        M32(LEG_CHILDREN + i * 4) = node;
        M32(node) = NODE_TABLE;
        M32(node + 0x8) = name;
        M32(node + 0x18) = NPC;
        MF32(node + 0x78) = points[i].x;
        MF32(node + 0x7c) = points[i].y;
        MF32(node + 0x80) = points[i].z;
    }

    Scene scene = {.root = ROOT, .generation = 1};
    const uint32_t npc = find_named_node(&scene, INTERNED_NPC);
    assert(npc == NPC);
    ClassicHeelPose pose = {.magnitude = 8.0f, .added_height = 0.5f,
                            .local_scale = MF32(npc + 0x50), .world_scale = MF32(npc + 0x84)};
    ClassicHeelPoint *const values[LEG_COUNT] = {&pose.left.foot, &pose.left.calf, &pose.right.foot, &pose.right.calf};
    for (unsigned i = 0; i < LEG_COUNT; ++i) {
        const uint32_t node = find_named_node(&scene, LEG_NAMES + i * 32);
        assert(node == LEG_NODES + i * 0x100 && M32(node + 0x18) == npc);
        *values[i] = (ClassicHeelPoint){MF32(node + 0x78), MF32(node + 0x7c), MF32(node + 0x80)};
    }

    ClassicHeelHeight height = {0};
    float target;
    assert(classic_heel_pose_height(&pose, &target) && fabsf(target - 4.793427f) < 0.00001f);
    assert(classic_heel_set_height(&height, target, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    assert(fabsf(MF32(NPC + 0x4c) - 14.793427f) < 0.00001f);
    assert(fabsf(MF32(NPC + 0x80) - 329.58685f) < 0.0001f);
    assert(classic_heel_set_height(&height, 0, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    assert(MF32(NPC + 0x4c) == 10.0f && MF32(NPC + 0x80) == 320.0f && scene.refreshes == 2);
}

int main(void)
{
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    assert(gm_commit_page(&space, STACK / space.page_size * space.page_size, NULL));
    MEM = space.base;
    M32(NODE_TABLE + 0x9c) = GET_NAME;
    M32(NODE_TABLE + 0xb4) = REFRESH;
    make_scene(ROOT, NPC, CHILDREN, 10.0f);
    make_scene(OTHER_ROOT, OTHER_NPC, OTHER_CHILDREN, 20.0f);
    Scene scene = {.root = ROOT, .generation = 1};
    ClassicHeelHeight height = {0};
    assert(classic_heel_set_height(&height, 3.0f, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    assert(MF32(NPC + 0x4c) == 13.0f && MF32(NPC + 0x80) == 326.0f);
    assert(MF32(NPC + 0x78) == 102.0f && MF32(NPC + 0x7c) == 206.0f);
    MF32(NPC + 0x4c) += 2.0f;
    assert(classic_heel_set_height(&height, 0.0f, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    assert(MF32(NPC + 0x4c) == 12.0f && MF32(NPC + 0x80) == 324.0f);

    assert(classic_heel_set_height(&height, 1.0f, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    scene.replace_before_commit = true;
    assert(classic_heel_set_height(&height, 2.0f, &scene, read_node, commit_node) == CLASSIC_HEEL_WRITE_FAILED);
    assert(height.applied_offset == 1.0f && height.root_generation == 1 && scene.refreshes == 3);
    assert(MF32(NPC + 0x4c) == 13.0f && MF32(OTHER_NPC + 0x4c) == 20.0f);
    assert(classic_heel_set_height(&height, 2.0f, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    assert(MF32(OTHER_NPC + 0x4c) == 22.0f && MF32(OTHER_NPC + 0x80) == 344.0f);
    assert(classic_heel_set_height(&height, 0.0f, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    assert(MF32(OTHER_NPC + 0x4c) == 20.0f && MF32(OTHER_NPC + 0x80) == 340.0f);
    assert(MF32(NPC + 0x4c) == 13.0f && scene.refreshes == 5);

    M32(OTHER_NPC + 0x8) = INTERNED_ROOT;
    assert(classic_heel_set_height(&height, 1.0f, &scene, read_node, commit_node) == CLASSIC_HEEL_NODE_UNAVAILABLE);
    assert(MF32(OTHER_NPC + 0x4c) == 20.0f && scene.refreshes == 5);
    test_pose_on_named_nodes();
    assert(gm_release(&space, NULL));
    puts("Classic scene recomposes owned local height and undo in the authored NiNode hierarchy");
    return 0;
}
