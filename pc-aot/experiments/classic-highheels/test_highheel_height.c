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

int main(void)
{
    equip_and_restore();
    preserve_other_offset();
    reset_on_root_replacement();
    reject_stale_write_and_retry();
    keep_ownership_on_failure();
    reject_bad_values();
    puts("6 Classic HighHeels host scenarios passed");
    return 0;
}
