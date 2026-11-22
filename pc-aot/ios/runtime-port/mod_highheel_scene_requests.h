/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "highheel_height.h"
#include "mod_highheel_native_host.h"

/* Native callers publish only validated scalars. Requests use the original
 * actor-wide Keep/Stop semantics and replace earlier values for that actor. */
bool skyrim_highheel_scene_queue_update(void *, const SkyrimHighHeelUpdate *);
void skyrim_highheel_scene_reset_requests(void);

/* Calculate for a verified scene receiver. The copied pose may include the
 * previous rendered offset for smoothing. Only the current request in the
 * current VM may publish the result. False leaves the output untouched. */
bool skyrim_highheel_scene_transform_offset(SkyrimVmScope, uint32_t actor, const ClassicHeelPose *, float *);

/* Call at the serialized scene update for this actor with a fresh pose and
 * a verified node read/commit pair. Keep one owned height per actor.
 * The commit callback only validates the node and stores its scalar value.
 * Scene traversal, recomposition and other guest calls stay outside it. */
ClassicHeelResult skyrim_highheel_scene_update_height(SkyrimVmScope scope, uint32_t actor,
                                                      const ClassicHeelPose *pose, ClassicHeelHeight *height,
                                                      void *scene, ClassicHeelReadNode read_node,
                                                      ClassicHeelCommitNode commit_node);
