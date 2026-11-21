/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

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
