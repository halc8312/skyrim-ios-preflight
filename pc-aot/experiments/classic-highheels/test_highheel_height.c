/* SPDX-License-Identifier: GPL-3.0-only */
#include "highheel_height.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct TestActor {
    ClassicHeelNode node;
    unsigned reads;
    unsigned writes;
    unsigned refreshes;
    bool available;
    bool reject_commit;
    bool switch_node_on_commit;
} TestActor;

static bool read_node(void *data, const char *name, ClassicHeelNode *node)
{
    TestActor *actor = data;
    assert(strcmp(name, "NPC") == 0);
    ++actor->reads;
    if (!actor->available)
        return false;
    *node = actor->node;
    return true;
}

static bool commit_node(void *data, const ClassicHeelNode *observed, float local_z)
{
    TestActor *actor = data;
    if (actor->switch_node_on_commit)
        ++actor->node.node_generation;
    if (actor->reject_commit ||
        actor->node.root_generation != observed->root_generation ||
        actor->node.node_generation != observed->node_generation ||
        actor->node.local_z != observed->local_z)
        return false;
    actor->node.local_z = local_z;
    ++actor->writes;
    ++actor->refreshes;
    return true;
}

static ClassicHeelResult set_height(TestActor *actor, ClassicHeelHeight *height, float desired)
{
    return classic_heel_set_height(height, desired, actor, read_node, commit_node);
}

static void equip_and_restore(void)
{
    TestActor actor = {.node = {10, 12, 6.0f}, .available = true};
    ClassicHeelHeight height = {0};
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_UNCHANGED);
    assert(actor.writes == 0);
    assert(set_height(&actor, &height, 4.0f) == CLASSIC_HEEL_APPLIED);
    assert(actor.node.local_z == 10.0f);
    assert(set_height(&actor, &height, 4.0f) == CLASSIC_HEEL_UNCHANGED);
    assert(set_height(&actor, &height, 2.0f) == CLASSIC_HEEL_APPLIED);
    assert(actor.node.local_z == 8.0f);
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_APPLIED);
    assert(actor.node.local_z == 6.0f);
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_UNCHANGED);
    assert(actor.writes == 3 && actor.refreshes == 3);
}

static void preserve_other_offset(void)
{
    TestActor actor = {.node = {10, 12, 6.0f}, .available = true};
    ClassicHeelHeight height = {0};
    assert(set_height(&actor, &height, 4.0f) == CLASSIC_HEEL_APPLIED);
    actor.node.local_z += 3.0f;
    assert(set_height(&actor, &height, 4.0f) == CLASSIC_HEEL_UNCHANGED);
    assert(set_height(&actor, &height, -2.0f) == CLASSIC_HEEL_APPLIED);
    assert(actor.node.local_z == 7.0f);
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_APPLIED);
    assert(actor.node.local_z == 9.0f);
}

static void reset_on_root_replacement(void)
{
    TestActor actor = {.node = {10, 12, 6.0f}, .available = true};
    ClassicHeelHeight height = {0};
    assert(set_height(&actor, &height, 4.0f) == CLASSIC_HEEL_APPLIED);
    actor.node = (ClassicHeelNode){11, 12, 9.0f};
    assert(set_height(&actor, &height, 4.0f) == CLASSIC_HEEL_APPLIED);
    assert(actor.node.local_z == 13.0f);
    actor.node = (ClassicHeelNode){20, 12, 3.0f};
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_UNCHANGED);
    assert(actor.node.local_z == 3.0f);
    assert(actor.writes == 2 && actor.refreshes == 2);
}

static void reject_stale_write_and_retry(void)
{
    TestActor actor = {.node = {10, 12, 6.0f}, .available = true};
    ClassicHeelHeight height = {0};
    assert(set_height(&actor, &height, 4.0f) == CLASSIC_HEEL_APPLIED);
    actor.node = (ClassicHeelNode){10, 12, 9.0f};
    actor.switch_node_on_commit = true;
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_WRITE_FAILED);
    assert(height.node_generation == 12 && height.applied_offset == 4.0f);
    actor.switch_node_on_commit = false;
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_UNCHANGED);
    assert(height.node_generation == 13 && actor.node.local_z == 9.0f);
    assert(actor.writes == 1 && actor.refreshes == 1);
}

static void keep_ownership_on_failure(void)
{
    TestActor actor = {.node = {10, 12, 6.0f}, .available = true};
    ClassicHeelHeight height = {0};
    assert(set_height(&actor, &height, 4.0f) == CLASSIC_HEEL_APPLIED);
    actor.available = false;
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_NODE_UNAVAILABLE);
    actor.available = true;
    actor.node.node_generation = 0;
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_NODE_UNAVAILABLE);
    actor.node.node_generation = 12;
    actor.reject_commit = true;
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_WRITE_FAILED);
    assert(actor.node.local_z == 10.0f && height.applied_offset == 4.0f);
    assert(actor.writes == 1 && actor.refreshes == 1);
    actor.reject_commit = false;
    assert(set_height(&actor, &height, 0.0f) == CLASSIC_HEEL_APPLIED);
    assert(actor.node.local_z == 6.0f);
}

static void reject_bad_values(void)
{
    TestActor actor = {.node = {10, 12, 6.0f}, .available = true};
    ClassicHeelHeight height = {0};
    assert(set_height(&actor, &height, NAN) == CLASSIC_HEEL_INVALID);
    assert(set_height(&actor, &height, INFINITY) == CLASSIC_HEEL_INVALID);
    assert(actor.reads == 0);
    actor.node.local_z = NAN;
    assert(set_height(&actor, &height, 1.0f) == CLASSIC_HEEL_INVALID);
    actor.node.local_z = FLT_MAX;
    assert(set_height(&actor, &height, FLT_MAX) == CLASSIC_HEEL_INVALID);
    assert(actor.writes == 0 && height.root_generation == 0);
}

static void calculate_pose_height(void)
{
    ClassicHeelPose pose = {
        .left = {.foot = {1.0f, 2.0f, -4.0f}, .calf = {1.0f, 2.0f, 26.0f}},
        .right = {.foot = {-7.0f, 1.0f, 2.0f}, .calf = {17.0f, 1.0f, 20.0f}},
        .local_scale = 2.0f, .world_scale = 4.0f, .magnitude = 8.0f, .added_height = 0.5f,
    };
    float height = 0.0f;
    assert(classic_heel_pose_height(&pose, &height));
    assert(fabsf(height - 4.793427f) < 0.00001f);
    pose.left = (ClassicHeelLeg){.calf = {3.0f, 4.0f, 0.0f}};
    pose.right = (ClassicHeelLeg){.calf = {3.0f, 4.0f, 12.0f}};
    pose.local_scale = pose.world_scale = 1.0f;
    assert(classic_heel_pose_height(&pose, &height));
    assert(fabsf(height - 2.877897f) < 0.00001f);
    pose.right.calf.z = -12.0f;
    assert(classic_heel_pose_height(&pose, &height));
    assert(height == 0.5f);
}

static void smooth_or_suspend_pose_height(void)
{
    ClassicHeelPose pose = {
        .left = {.calf = {0, 0, 30}}, .right = {.calf = {0, 0, 30}},
        .local_scale = 1, .world_scale = 1, .magnitude = 8,
        .previous_height = 2, .max_step = 0.125f, .smoothing_mode = 1,
    };
    float height = 0;
    assert(classic_heel_pose_height(&pose, &height) && height > 8.0f);
    pose.smooth_this_state = true;
    assert(classic_heel_pose_height(&pose, &height) && height == 2.125f);
    pose.smoothing_mode = 2;
    pose.smooth_this_state = false;
    pose.magnitude = 0;
    assert(classic_heel_pose_height(&pose, &height) && height == 1.875f);
    pose.suspended = true;
    assert(classic_heel_pose_height(&pose, &height) && height == 0);
}

static void reject_invalid_pose_inputs(void)
{
    ClassicHeelPose pose = {.left = {.calf = {0, 0, 1}}, .right = {.calf = {0, 0, 1}},
                            .local_scale = 1, .world_scale = 0, .magnitude = 1};
    float height = 98.0f;
    assert(!classic_heel_pose_height(&pose, &height) && height == 98.0f);
    pose.world_scale = 1;
    pose.left.calf.z = 0;
    assert(!classic_heel_pose_height(&pose, &height) && height == 98.0f);
    pose.left.calf.z = INFINITY;
    assert(!classic_heel_pose_height(&pose, &height) && height == 98.0f);
    pose.left.calf.z = 1;
    pose.magnitude = NAN;
    assert(!classic_heel_pose_height(&pose, &height) && height == 98.0f);
    pose.magnitude = 1;
    pose.smoothing_mode = 2;
    pose.max_step = -0.125f;
    assert(!classic_heel_pose_height(&pose, &height) && height == 98.0f);
}

int main(void)
{
    equip_and_restore();
    preserve_other_offset();
    reset_on_root_replacement();
    reject_stale_write_and_retry();
    keep_ownership_on_failure();
    reject_bad_values();
    calculate_pose_height();
    smooth_or_suspend_pose_height();
    reject_invalid_pose_inputs();
    puts("9 Classic HighHeels host scenarios passed");
    return 0;
}
