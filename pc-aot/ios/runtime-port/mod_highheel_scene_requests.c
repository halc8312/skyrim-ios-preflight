/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_highheel_scene_requests.h"
#include "mod_highheel_effect_host.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;
static SkyrimVmScope request_scope;
static SkyrimHighHeelUpdate *rows;
static size_t count, capacity;

static size_t actor_index(uint32_t actor) {
    for (size_t i = 0; i < count; ++i)
        if (rows[i].actor == actor) return i;
    return count;
}

static bool queue(const SkyrimHighHeelUpdate *update) {
    if (update->scope.epoch < request_scope.epoch) return false;
    if (update->scope.epoch > request_scope.epoch) {
        count = 0;
        request_scope = update->scope;
    }
    if (update->scope.registry != request_scope.registry) return false;
    size_t index = actor_index(update->actor);
    if (!update->keep) {
        if (index != count) rows[index] = rows[--count];
        return true;
    }
    if (index == count) {
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2 / sizeof *rows) return false;
            const size_t expanded = capacity ? capacity * 2 : 8;
            SkyrimHighHeelUpdate *next = realloc(rows, expanded * sizeof *rows);
            if (!next) return false;
            rows = next;
            capacity = expanded;
        }
        ++count;
    }
    rows[index] = *update;
    return true;
}

bool skyrim_highheel_scene_queue_update(void *unused, const SkyrimHighHeelUpdate *update) {
    (void)unused;
    if (!update || !update->scope.epoch || !update->scope.registry || !update->actor || !update->handle ||
        (uint32_t)update->handle != update->actor) return false;
    pthread_mutex_lock(&request_lock);
    const bool queued = queue(update);
    pthread_mutex_unlock(&request_lock);
    return queued;
}

void skyrim_highheel_scene_reset_requests(void) {
    pthread_mutex_lock(&request_lock);
    free(rows);
    rows = NULL;
    count = capacity = 0;
    request_scope = (SkyrimVmScope){0};
    pthread_mutex_unlock(&request_lock);
}

static SkyrimHighHeelUpdate snapshot_locked(SkyrimVmScope scope, uint32_t actor) {
    if (scope.epoch == request_scope.epoch && scope.registry == request_scope.registry) {
        const size_t index = actor_index(actor);
        if (index != count) return rows[index];
    }
    return (SkyrimHighHeelUpdate){0};
}

static SkyrimHighHeelUpdate snapshot(SkyrimVmScope scope, uint32_t actor) {
    pthread_mutex_lock(&request_lock);
    const SkyrimHighHeelUpdate request = snapshot_locked(scope, actor);
    pthread_mutex_unlock(&request_lock);
    return request;
}

static void copy_magnitude(ClassicHeelPose *pose, const SkyrimHighHeelUpdate *request) {
    _Static_assert(sizeof pose->magnitude == sizeof request->magnitude, "Classic Float width");
    memcpy(&pose->magnitude, &request->magnitude, sizeof pose->magnitude);
}

typedef struct {
    SkyrimHighHeelUpdate request;
    float value, *output;
} RequestedOffset;

static bool publish_offset(void *context) {
    const RequestedOffset *offset = context;
    const SkyrimHighHeelUpdate *request = &offset->request;
    pthread_mutex_lock(&request_lock);
    const SkyrimHighHeelUpdate current = snapshot_locked(request->scope, request->actor);
    const bool same = current.handle == request->handle && current.magnitude == request->magnitude;
    if (same) *offset->output = offset->value;
    pthread_mutex_unlock(&request_lock);
    return same;
}

bool skyrim_highheel_scene_transform_offset(SkyrimVmScope scope, uint32_t actor, const ClassicHeelPose *pose,
                                           float *output) {
    if (!actor || !pose || !output) return false;
    const SkyrimHighHeelUpdate request = snapshot(scope, actor);
    if (!request.keep) return false;
    ClassicHeelPose current = *pose;
    copy_magnitude(&current, &request);
    RequestedOffset offset = {.request = request, .output = output};
    if (!classic_heel_pose_height(&current, &offset.value) || !skyrim_vm_commit_begin(&scope)) return false;
    const bool accepted = skyrim_highheel_effect_commit_if_matches(scope, request.handle, actor, true,
                                                                    request.magnitude, publish_offset, &offset);
    skyrim_vm_commit_end();
    return accepted;
}

typedef struct {
    SkyrimVmScope scope;
    uint32_t actor;
    SkyrimHighHeelUpdate request;
    bool apply;
    void *scene;
    ClassicHeelReadNode read_node;
    ClassicHeelCommitNode commit_node;
} SceneUpdate;

static bool read_for_update(void *context, const char *name, ClassicHeelNode *node) {
    const SceneUpdate *update = context;
    return update->read_node(update->scene, name, node);
}

typedef struct {
    const SceneUpdate *update;
    const ClassicHeelNode *node;
    float local_z;
} SceneWrite;

static bool commit_if_requested(void *context) {
    const SceneWrite *write = context;
    const SceneUpdate *update = write->update;
    pthread_mutex_lock(&request_lock);
    const SkyrimHighHeelUpdate current = snapshot_locked(update->scope, update->actor);
    const bool applied = current.handle == update->request.handle && current.magnitude == update->request.magnitude &&
                         update->commit_node(update->scene, write->node, write->local_z);
    pthread_mutex_unlock(&request_lock);
    return applied;
}

static bool commit_if_current(void *context, const ClassicHeelNode *node, float local_z) {
    const SceneUpdate *update = context;
    if (!skyrim_vm_commit_begin(&update->scope)) return false;
    SceneWrite write = {update, node, local_z};
    // An ended handle cannot reactivate in this epoch. A new Keep must replace the row.
    const bool applied = update->apply
        ? skyrim_highheel_effect_commit_if_matches(update->scope, update->request.handle, update->actor,
                                                   true, update->request.magnitude, commit_if_requested, &write)
        : commit_if_requested(&write);
    skyrim_vm_commit_end();
    return applied;
}

ClassicHeelResult skyrim_highheel_scene_update_height(SkyrimVmScope scope, uint32_t actor,
                                                      const ClassicHeelPose *pose, ClassicHeelHeight *height,
                                                      void *scene, ClassicHeelReadNode read_node,
                                                      ClassicHeelCommitNode commit_node) {
    if (!actor || !height || !read_node || !commit_node) return CLASSIC_HEEL_INVALID;
    const SkyrimHighHeelUpdate request = snapshot(scope, actor);
    const bool apply = request.keep && skyrim_highheel_effect_matches(scope, request.handle, actor, true,
                                                                      request.magnitude);
    SceneUpdate update = {scope, actor, request, apply, scene, read_node, commit_node};
    if (apply) {
        if (!pose) return CLASSIC_HEEL_NODE_UNAVAILABLE;
        ClassicHeelPose current = *pose;
        copy_magnitude(&current, &request);
        return classic_heel_set_pose_height(height, &current, &update, read_for_update, commit_if_current);
    }
    if (height->applied_offset == 0.0f) return CLASSIC_HEEL_UNCHANGED;
    return classic_heel_set_height(height, 0.0f, &update, read_for_update, commit_if_current);
}
