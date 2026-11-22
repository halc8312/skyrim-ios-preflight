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

static void refresh_node(uint32_t node)
{
    CPU cpu = {.r = {0, node, 0, 0, STACK}};
    M32(STACK + 4) = CONTEXT;
    f_AB53E0(&cpu);
    assert(cpu.r[4] == STACK + 8);
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
    refresh_node(node);
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
    assert(classic_heel_set_pose_height(&height, &pose, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    assert(fabsf(MF32(NPC + 0x4c) - 14.793427f) < 0.00001f);
    assert(fabsf(MF32(NPC + 0x80) - 329.58685f) < 0.0001f);
    assert(classic_heel_set_height(&height, 0, &scene, read_node, commit_node) == CLASSIC_HEEL_APPLIED);
    assert(MF32(NPC + 0x4c) == 10.0f && MF32(NPC + 0x80) == 320.0f && scene.refreshes == 2);
}

#ifdef SKYRIM_TEST_HEEL_LOCAL_ARGUMENT
static float local_offset;
static unsigned accepted;

bool fixture_local_z(const CPU *cpu, uint32_t node, uint32_t local_z, uint32_t *out)
{
    assert(cpu && out);
    if (node != NPC || local_offset == 0.0f) return false;
    float before;
    memcpy(&before, &local_z, sizeof before);
    const float proposed = before + local_offset;
    if (!isfinite(proposed)) return false;
    memcpy(out, &proposed, sizeof proposed);
    ++accepted;
    return true;
}

static void test_local_transform_argument(void)
{
    make_scene(ROOT, NPC, CHILDREN, 10.0f);
    make_scene(OTHER_ROOT, OTHER_NPC, OTHER_CHILDREN, 20.0f);
    /* A rotated, scaled parent makes a local Z change affect world X. */
    MF32(ROOT + 0x54) = MF32(ROOT + 0x74) = 0.0f;
    MF32(ROOT + 0x5c) = 1.0f;
    MF32(ROOT + 0x6c) = -1.0f;
    for (unsigned i = 0; i < 2; ++i) {
        const float offset = i ? -2.5f : 4.25f;
        MF32(NPC + 0x4c) = 10.0f + offset;
        refresh_node(NPC);
        uint8_t expected[0x34], local[0x34];
        memcpy(expected, MEM + NPC + 0x54, sizeof expected);
        assert(MF32(NPC + 0x78) == 100.0f + 2.0f * (10.0f + offset));
        MF32(NPC + 0x4c) = 10.0f;
        memcpy(local, MEM + NPC + 0x20, sizeof local);
        local_offset = offset;
        for (unsigned pass = 0; pass < 2; ++pass) {
            refresh_node(NPC);
            assert(!memcmp(expected, MEM + NPC + 0x54, sizeof expected));
            assert(!memcmp(local, MEM + NPC + 0x20, sizeof local));
        }
        local_offset = 0.0f;
        refresh_node(NPC);
        assert(MF32(NPC + 0x78) == 120.0f);
    }
    assert(accepted == 4);
    local_offset = 17.0f;
    refresh_node(OTHER_NPC);
    assert(MF32(OTHER_NPC + 0x80) == 340.0f && accepted == 4);
    M32(NPC + 0x98) = 0x200; /* Stock parent-copy branch. */
    refresh_node(NPC);
    assert(!memcmp(MEM + ROOT + 0x54, MEM + NPC + 0x54, 0x34) && accepted == 4);
}
#endif

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
#ifdef SKYRIM_TEST_HEEL_LOCAL_ARGUMENT
    test_local_transform_argument();
#endif
    assert(gm_release(&space, NULL));
    puts("Classic scene recomposes owned local height and undo in the authored NiNode hierarchy");
    return 0;
}
