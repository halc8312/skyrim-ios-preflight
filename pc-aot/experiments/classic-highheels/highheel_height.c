/* SPDX-License-Identifier: GPL-3.0-only */
#include "highheel_height.h"

#include <math.h>

#ifdef __clang__
#pragma clang fp contract(off)
#endif

static float leg_length(ClassicHeelLeg leg, float *rise) {
    const float x = leg.calf.x - leg.foot.x;
    const float y = leg.calf.y - leg.foot.y;
    const float z = leg.calf.z - leg.foot.z;
    *rise = z;
    return sqrtf((x * x + z * z) + y * y);
}

bool classic_heel_pose_height(const ClassicHeelPose *pose, float *height) {
    if (!pose || !height || pose->smoothing_mode > 2) return false;
    if (pose->swimming) {
        *height = 0.0f;
        return true;
    }

    float left_rise, right_rise;
    const float left = leg_length(pose->left, &left_rise);
    const float right = leg_length(pose->right, &right_rise);
    if (!isfinite(left) || !isfinite(right) || left <= 0.0f || right <= 0.0f ||
        !isfinite(pose->local_scale) || !isfinite(pose->world_scale) ||
        pose->local_scale <= 0.0f || pose->world_scale <= 0.0f) return false;

    const float ratio = fmaxf(0.0f, fmaxf(left_rise / left, right_rise / right));
    const float scale = (left + right) / 0x1.bf31f8p+5f * pose->local_scale / pose->world_scale;
    float target = scale * pose->magnitude * ratio + pose->added_height;
    if (!isfinite(target)) return false;

    if (pose->smoothing_mode == 2 || (pose->smoothing_mode == 1 && pose->smooth_this_state)) {
        const float low = pose->previous_height - pose->max_step;
        const float high = pose->previous_height + pose->max_step;
        if (!isfinite(low) || !isfinite(high) || pose->max_step < 0.0f) return false;
        target = fminf(high, fmaxf(low, target));
    }
    *height = target;
    return true;
}

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
