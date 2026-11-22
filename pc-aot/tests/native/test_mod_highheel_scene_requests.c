/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_highheel_scene_requests.h"
#include "mod_highheel_effect_host.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum { FIRST_ACTOR = 0x14, SECOND_ACTOR = 0x31 };

typedef struct {
    SkyrimHighHeelUpdate update;
    bool active;
} Effect;

typedef struct {
    ClassicHeelNode node;
    const Effect *keep_during_read;
    Effect *end_during_read;
    bool available, refuse, retire_during_read;
    unsigned writes, attempts;
} Scene;

static const SkyrimVmScope initial = {7, 0x11200};
static SkyrimVmScope current;
static Effect effects[4];
static unsigned effect_checks;
static uint64_t checked_handle;
static bool vm_committing;

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void setup(void) {
    skyrim_highheel_scene_reset_requests();
    current = initial;
    memset(effects, 0, sizeof effects);
    effect_checks = 0;
    checked_handle = 0;
    assert(!vm_committing);
}

static Effect *start_effect(unsigned slot, uint32_t actor, unsigned number, float magnitude) {
    assert(slot < sizeof effects / sizeof effects[0]);
    Effect *effect = &effects[slot];
    *effect = (Effect){.update = {.scope = current, .handle = ((uint64_t)(0x20000u | number) << 32) | actor,
                                 .actor = actor, .magnitude = float_bits(magnitude), .keep = true},
                       .active = true};
    return effect;
}

/* Model the existing native boundary's independent receipt validation. */
bool skyrim_highheel_effect_matches(SkyrimVmScope scope, uint64_t handle, uint32_t actor, bool keep,
                                    uint32_t magnitude) {
    if (scope.epoch != current.epoch || scope.registry != current.registry) return false;
    ++effect_checks;
    checked_handle = handle;
    for (size_t i = 0; i < sizeof effects / sizeof effects[0]; ++i) {
        const Effect *effect = &effects[i];
        const SkyrimHighHeelUpdate *update = &effect->update;
        if (update->actor == actor && update->handle == handle &&
            update->scope.epoch == scope.epoch && update->scope.registry == scope.registry)
            return keep ? effect->active && update->magnitude == magnitude : magnitude == 0;
    }
    return false;
}

bool skyrim_vm_commit_begin(const SkyrimVmScope *scope) {
    assert(!vm_committing);
    return vm_committing = scope->epoch == current.epoch && scope->registry == current.registry;
}

void skyrim_vm_commit_end(void) {
    assert(vm_committing);
    vm_committing = false;
}

bool skyrim_highheel_effect_commit_if_matches(SkyrimVmScope scope, uint64_t handle, uint32_t actor, bool keep,
                                              uint32_t magnitude, bool (*action)(void *), void *context) {
    assert(vm_committing);
    return skyrim_highheel_effect_matches(scope, handle, actor, keep, magnitude) && action(context);
}

static void submit(const Effect *effect, bool keep) {
    SkyrimHighHeelUpdate update = effect->update;
    update.keep = keep;
    if (!keep) update.magnitude = 0;
    assert(skyrim_highheel_effect_matches(update.scope, update.handle, update.actor, keep, update.magnitude));
    assert(skyrim_highheel_scene_queue_update(NULL, &update));
}

static Scene scene_at(float local_z) {
    return (Scene){.node = {1, 1, local_z}, .available = true};
}

static bool read_node(void *context, const char *name, ClassicHeelNode *out) {
    Scene *scene = context;
    assert(!strcmp(name, "NPC"));
    if (!scene->available) return false;
    *out = scene->node;
    if (scene->keep_during_read) {
        submit(scene->keep_during_read, true);
        scene->keep_during_read = NULL;
    }
    if (scene->end_during_read) {
        scene->end_during_read->active = false;
        scene->end_during_read = NULL;
    }
    if (scene->retire_during_read) {
        ++current.epoch;
        scene->retire_during_read = false;
    }
    return true;
}

static bool commit_node(void *context, const ClassicHeelNode *observed, float local_z) {
    Scene *scene = context;
    assert(vm_committing);
    ++scene->attempts;
    assert(observed->root_generation == scene->node.root_generation);
    assert(observed->node_generation == scene->node.node_generation && observed->local_z == scene->node.local_z);
    if (scene->refuse) return false;
    ++scene->writes;
    scene->node.local_z = local_z;
    return true;
}

/* A vertical pair with the stock normalization length makes the offset equal
 * the magnitude. Give the pose a different magnitude to check queue ownership. */
static ClassicHeelPose standing(void) {
    const ClassicHeelLeg leg = {.calf = {.z = 0x1.bf31f8p+4f}};
    return (ClassicHeelPose){.left = leg, .right = leg, .local_scale = 1, .world_scale = 1, .magnitude = 99};
}

static ClassicHeelResult tick(uint32_t actor, const ClassicHeelPose *pose, ClassicHeelHeight *owned, Scene *scene) {
    return skyrim_highheel_scene_update_height(current, actor, pose, owned, scene, read_node, commit_node);
}

static void check_position(const Scene *scene, float expected) {
    assert(fabsf(scene->node.local_z - expected) < 0.0001f);
}

static void test_actor_coalescing_and_stop(void) {
    setup();
    Scene first = scene_at(20), second = scene_at(50);
    ClassicHeelHeight first_owned = {0}, second_owned = {0};
    const ClassicHeelPose pose = standing();
    Effect *earlier = start_effect(0, FIRST_ACTOR, 5, 6);
    const Effect *latest = start_effect(1, FIRST_ACTOR, 7, 12);
    const Effect *other = start_effect(2, SECOND_ACTOR, 7, 4);
    const Effect *sitting = start_effect(3, FIRST_ACTOR, 12, 8);
    submit(earlier, true);
    submit(other, true);
    submit(latest, true);
    earlier->active = false;

    effect_checks = 0;
    assert(tick(FIRST_ACTOR, &pose, &first_owned, &first) == CLASSIC_HEEL_APPLIED);
    assert(effect_checks == 2 && checked_handle == latest->update.handle);
    assert(first.writes == 1);
    check_position(&first, 32);
    assert(tick(SECOND_ACTOR, &pose, &second_owned, &second) == CLASSIC_HEEL_APPLIED);
    check_position(&second, 54);

    first.node.local_z += 3; /* Another scene owner changed the baseline. */
    submit(latest, true);
    assert(tick(FIRST_ACTOR, &pose, &first_owned, &first) == CLASSIC_HEEL_UNCHANGED);
    assert(first.writes == 1);
    check_position(&first, 35);

    /* Stock Stop clears the actor even when a different live effect issued Keep. */
    submit(sitting, false);
    assert(tick(FIRST_ACTOR, NULL, &first_owned, &first) == CLASSIC_HEEL_APPLIED);
    assert(first.writes == 2 && first_owned.applied_offset == 0);
    check_position(&first, 23);
    assert(tick(SECOND_ACTOR, &pose, &second_owned, &second) == CLASSIC_HEEL_UNCHANGED);
    assert(second.writes == 1);
    check_position(&second, 54);
}

static void test_effect_finish_and_new_epoch(void) {
    setup();
    Scene scene = scene_at(10);
    ClassicHeelHeight owned = {0};
    const ClassicHeelPose pose = standing();
    Effect *effect = start_effect(0, FIRST_ACTOR, 4, 8);
    submit(effect, true);
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 18);

    effect->active = false;
    assert(tick(FIRST_ACTOR, NULL, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 10);

    effect = start_effect(1, FIRST_ACTOR, 8, 5);
    submit(effect, true);
    const SkyrimHighHeelUpdate stale = effect->update;
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 15);
    ++current.epoch;
    assert(tick(FIRST_ACTOR, NULL, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 10);

    submit(start_effect(2, FIRST_ACTOR, 8, 3), true);
    (void)skyrim_highheel_scene_queue_update(NULL, &stale);
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 13);
}

static void test_replacement_matched_pose_and_retry(void) {
    setup();
    Scene scene = scene_at(100);
    ClassicHeelHeight owned = {0};
    ClassicHeelPose pose = standing();
    const Effect *first = start_effect(0, FIRST_ACTOR, 7, 10);
    submit(first, true);
    const ClassicHeelResult absent = tick(FIRST_ACTOR, NULL, &owned, &scene);
    assert(absent != CLASSIC_HEEL_APPLIED && !scene.attempts && owned.applied_offset == 0);
    ClassicHeelPose invalid = pose;
    invalid.world_scale = NAN;
    assert(tick(FIRST_ACTOR, &invalid, &owned, &scene) == CLASSIC_HEEL_INVALID);
    assert(!scene.attempts);
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 110);

    const Effect *latest = start_effect(1, FIRST_ACTOR, 8, 15);
    submit(latest, true);
    scene.refuse = true;
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_WRITE_FAILED);
    assert(owned.applied_offset == 10 && scene.writes == 1);
    check_position(&scene, 110);
    scene.refuse = false;
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    assert(scene.writes == 2);
    check_position(&scene, 115);

    /* Replacement in the same root uses a fresh smoothing baseline. */
    scene.node.node_generation = 9;
    scene.node.local_z = 150;
    pose.smoothing_mode = 2;
    pose.max_step = 3;
    pose.previous_height = 200; /* Caller-provided previous offsets are stale. */
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 153);
    assert(owned.root_generation == 1 && owned.node_generation == 9 && owned.applied_offset == 3);
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 156);
    submit(latest, false);
    assert(tick(FIRST_ACTOR, NULL, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 150);
}

static void test_newer_keep_arrives_while_reading_node(void) {
    setup();
    Scene scene = scene_at(66);
    ClassicHeelHeight owned = {0};
    const ClassicHeelPose pose = standing();
    submit(start_effect(0, FIRST_ACTOR, 7, 10), true);
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 76);

    submit(start_effect(1, FIRST_ACTOR, 8, 13), true);
    scene.keep_during_read = start_effect(2, FIRST_ACTOR, 9, 16);
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_WRITE_FAILED);
    assert(scene.attempts == 1 && scene.writes == 1);
    check_position(&scene, 76);

    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    assert(scene.attempts == 2 && scene.writes == 2);
    check_position(&scene, 82);
}

static void test_scope_or_effect_ends_while_reading_node(void) {
    setup();
    Scene scene = scene_at(10);
    ClassicHeelHeight owned = {0};
    const ClassicHeelPose pose = standing();
    submit(start_effect(0, FIRST_ACTOR, 7, 8), true);
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_APPLIED);

    Effect *latest = start_effect(1, FIRST_ACTOR, 8, 13);
    submit(latest, true);
    scene.end_during_read = latest;
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_WRITE_FAILED);
    assert(scene.attempts == 1 && owned.applied_offset == 8);
    check_position(&scene, 18);
    assert(tick(FIRST_ACTOR, NULL, &owned, &scene) == CLASSIC_HEEL_APPLIED);
    check_position(&scene, 10);

    submit(start_effect(2, FIRST_ACTOR, 9, 16), true);
    scene.retire_during_read = true;
    assert(tick(FIRST_ACTOR, &pose, &owned, &scene) == CLASSIC_HEEL_WRITE_FAILED);
    assert(scene.attempts == 2 && owned.applied_offset == 0);
    check_position(&scene, 10);
}

static void test_transform_offset_is_a_current_effect_only(void) {
    setup();
    ClassicHeelPose pose = standing();
    float offset = -9.0f;
    assert(!skyrim_highheel_scene_transform_offset(current, FIRST_ACTOR, &pose, &offset) && offset == -9.0f);

    Effect *effect = start_effect(0, FIRST_ACTOR, 7, 4.0f);
    submit(effect, true);
    pose.previous_height = 1.0f;
    pose.max_step = 0.5f;
    pose.smoothing_mode = 2;
    assert(skyrim_highheel_scene_transform_offset(current, FIRST_ACTOR, &pose, &offset) && offset == 1.5f);
    assert(!skyrim_highheel_scene_transform_offset(current, SECOND_ACTOR, &pose, &offset) && offset == 1.5f);
    pose.swimming = true;
    assert(skyrim_highheel_scene_transform_offset(current, FIRST_ACTOR, &pose, &offset) && offset == 0.0f);
    pose.swimming = false;
    effect->active = false;
    assert(!skyrim_highheel_scene_transform_offset(current, FIRST_ACTOR, &pose, &offset) && offset == 0.0f);
    submit(effect, false);
    assert(!skyrim_highheel_scene_transform_offset(current, FIRST_ACTOR, &pose, &offset) && offset == 0.0f);
}

int main(void) {
    test_actor_coalescing_and_stop();
    test_effect_finish_and_new_epoch();
    test_replacement_matched_pose_and_retry();
    test_newer_keep_arrives_while_reading_node();
    test_scope_or_effect_ends_while_reading_node();
    test_transform_offset_is_a_current_effect_only();
    skyrim_highheel_scene_reset_requests();
    return 0;
}
