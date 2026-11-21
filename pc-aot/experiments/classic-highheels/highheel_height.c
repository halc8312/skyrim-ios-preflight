/* SPDX-License-Identifier: GPL-3.0-only */
#include "highheel_height.h"

#include <math.h>

ClassicHeelResult classic_heel_set_height(ClassicHeelHeight *height, float desired_offset,
                                         void *actor, ClassicHeelReadNode read_node,
                                         ClassicHeelCommitNode commit_node)
{
    if (!height || !read_node || !commit_node || !isfinite(desired_offset))
        return CLASSIC_HEEL_INVALID;

    ClassicHeelNode node = {0};
    if (!read_node(actor, "NPC", &node) || !node.root_generation || !node.node_generation)
        return CLASSIC_HEEL_NODE_UNAVAILABLE;
    if (!isfinite(node.local_z))
        return CLASSIC_HEEL_INVALID;

    bool same_node = height->root_generation == node.root_generation &&
                     height->node_generation == node.node_generation;
    float previous_offset = same_node ? height->applied_offset : 0.0f;
    float delta = desired_offset - previous_offset;
    float local_z = node.local_z + delta;
    if (!isfinite(delta) || !isfinite(local_z))
        return CLASSIC_HEEL_INVALID;

    if (delta != 0.0f && !commit_node(actor, &node, local_z))
        return CLASSIC_HEEL_WRITE_FAILED;

    *height = (ClassicHeelHeight){node.root_generation, node.node_generation, desired_offset};
    return delta != 0.0f ? CLASSIC_HEEL_APPLIED : CLASSIC_HEEL_UNCHANGED;
}
