/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float x, y, z;
} ClassicHeelPoint;

typedef struct {
    ClassicHeelPoint foot, calf; /* world positions from the same scene update */
} ClassicHeelLeg;

typedef struct {
    ClassicHeelLeg left, right;
    float local_scale, world_scale, magnitude, added_height;
    float previous_height, max_step;
    unsigned smoothing_mode;
    bool smooth_this_state, swimming;
} ClassicHeelPose;

/* Compute the fixed beta 0.5 offset from copied pose values. Invalid input
 * leaves the output alone. Modes 0, 1 and 2 are unbounded, state-dependent
 * smoothing and unconditional smoothing, respectively. */
bool classic_heel_pose_height(const ClassicHeelPose *pose, float *height);

typedef struct ClassicHeelNode {
    uint64_t root_generation;
    uint64_t node_generation;
    float local_z;
} ClassicHeelNode;

typedef struct ClassicHeelHeight {
    uint64_t root_generation;
    uint64_t node_generation;
    float applied_offset;
} ClassicHeelHeight;

typedef enum ClassicHeelResult {
    CLASSIC_HEEL_APPLIED,
    CLASSIC_HEEL_UNCHANGED,
    CLASSIC_HEEL_NODE_UNAVAILABLE,
    CLASSIC_HEEL_INVALID,
    CLASSIC_HEEL_WRITE_FAILED,
} ClassicHeelResult;

typedef bool (*ClassicHeelReadNode)(void *actor, const char *name, ClassicHeelNode *node);
/* True means the same node and position were updated and refresh was accepted.
 * False leaves the position untouched, including on a stale observation. */
typedef bool (*ClassicHeelCommitNode)(void *actor, const ClassicHeelNode *observed, float local_z);

/* Initialize height to zero for each actor. Use the verified, serialized update
 * thread and unique nonzero identities for each publication of a root and node.
 * desired_offset == 0 removes this effect. Retain height to retry failures. */
ClassicHeelResult classic_heel_set_height(ClassicHeelHeight *height, float desired_offset,
                                         void *actor, ClassicHeelReadNode read_node,
                                         ClassicHeelCommitNode commit_node);
/* The copied pose and verified node must come from the same owner update.
 * Smoothing starts at the owned height of that node's current generation. */
ClassicHeelResult classic_heel_set_pose_height(ClassicHeelHeight *height, const ClassicHeelPose *pose,
                                              void *actor, ClassicHeelReadNode read_node,
                                              ClassicHeelCommitNode commit_node);
