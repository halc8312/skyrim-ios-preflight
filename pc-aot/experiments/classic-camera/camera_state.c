/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera_state.h"
#include <string.h>

static const char image_sha256_1932[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";

enum {
    PLAYER_CAMERA_SINGLETON = 0x012E7288,
    CURRENT_STATE_OFFSET = 0x20,
    PLAYER_STATE_TABLE_OFFSET = 0x6C,
    PLAYER_STATE_COUNT = 13,
    STATE_TABLE_LAST_BYTE = PLAYER_STATE_TABLE_OFFSET + 4 * PLAYER_STATE_COUNT - 1,
    FIRST_PERSON_SLOT = 0,
    THIRD_PERSON_SLOT_1 = 8,
    THIRD_PERSON_SLOT_2 = 9,

    // Classic SKSE 1.7.3 GameAPI, GameReferences and NiObjects layouts.
    PLAYER_ACTOR_SINGLETON = 0x01B2E8E4,
    ACTOR_LOADED_STATE = 0x44,
    LOADED_STATE_ROOT = 0x20,
    ACTOR_FIRST_PERSON_ROOT = 0x58C,
    SCENE_ROOT_PARENT = 0x18,
    SCENE_ROOT_FLAGS = 0x98,
    SCENE_ROOT_CULL_BIT = 1,
    GUEST_FIRST_MAPPED_ADDRESS = 0x10000
};

bool classic_camera_image_supported(const char *image_sha256) {
    return image_sha256 && !strcmp(image_sha256, image_sha256_1932);
}

int classic_camera_state_after_return(const char *image_sha256, uint32_t completed_entry,
                                      uint32_t receiver, ClassicCameraReadWord read_word, void *guest) {
    if (!classic_camera_image_supported(image_sha256) || !read_word ||
        completed_entry != CLASSIC_CAMERA_SET_STATE || !receiver || receiver > UINT32_MAX - STATE_TABLE_LAST_BYTE)
        return -1;

    uint32_t player, current;
    if (!read_word(guest, PLAYER_CAMERA_SINGLETON, &player) || player != receiver ||
        !read_word(guest, player + CURRENT_STATE_OFFSET, &current) || !current)
        return -1;

    for (int slot = 0; slot < PLAYER_STATE_COUNT; ++slot) {
        uint32_t state;
        if (!read_word(guest, player + PLAYER_STATE_TABLE_OFFSET + 4u * (unsigned)slot, &state)) return -1;
        if (state == current) return slot;
    }
    return -1;
}

static bool valid_object_address(uint32_t object) {
    return object >= GUEST_FIRST_MAPPED_ADDRESS && object % sizeof(uint32_t) == 0;
}

static bool read_field(ClassicCameraReadWord read_word, void *guest, uint32_t object,
                       uint32_t offset, uint32_t *value) {
    if (!valid_object_address(object) || object > UINT32_MAX - offset - (sizeof(uint32_t) - 1))
        return false;
    return read_word(guest, object + offset, value);
}

static ClassicCameraRootState read_root(ClassicCameraReadWord read_word, void *guest, uint32_t root) {
    if (!root) return CLASSIC_CAMERA_ROOT_ABSENT;
    uint32_t parent, flags;
    if (!read_field(read_word, guest, root, SCENE_ROOT_PARENT, &parent) ||
        (parent && !valid_object_address(parent)) ||
        !read_field(read_word, guest, root, SCENE_ROOT_FLAGS, &flags))
        return CLASSIC_CAMERA_ROOT_UNAVAILABLE;
    if (parent)
        return (flags & SCENE_ROOT_CULL_BIT) ? CLASSIC_CAMERA_ROOT_ATTACHED_CULL
                                           : CLASSIC_CAMERA_ROOT_ATTACHED_CLEAR;
    return (flags & SCENE_ROOT_CULL_BIT) ? CLASSIC_CAMERA_ROOT_DETACHED_CULL
                                       : CLASSIC_CAMERA_ROOT_DETACHED_CLEAR;
}

bool classic_camera_sample_after_return(const char *image_sha256, uint32_t completed_entry,
                                        uint32_t receiver, ClassicCameraReadWord read_word, void *guest,
                                        ClassicCameraSample *sample) {
    if (!sample) return false;
    const int slot = classic_camera_state_after_return(image_sha256, completed_entry, receiver, read_word, guest);
    if (slot < 0) return false;
    *sample = (ClassicCameraSample){
        .slot = slot,
        .sampled_roots = slot == FIRST_PERSON_SLOT || slot == THIRD_PERSON_SLOT_1 || slot == THIRD_PERSON_SLOT_2
    };
    if (!sample->sampled_roots) return true;

    uint32_t actor, loaded_state;
    if (!read_word(guest, PLAYER_ACTOR_SINGLETON, &actor) || !actor) return true;
    uint32_t arms = 0, body = 0;
    const bool have_arms = read_field(read_word, guest, actor, ACTOR_FIRST_PERSON_ROOT, &arms);
    const bool have_body = read_field(read_word, guest, actor, ACTOR_LOADED_STATE, &loaded_state) &&
        (!loaded_state || read_field(read_word, guest, loaded_state, LOADED_STATE_ROOT, &body));
    if (have_arms && have_body && arms && arms == body) return true;
    if (have_arms) sample->arms = read_root(read_word, guest, arms);
    if (have_body) sample->body = read_root(read_word, guest, body);
    return true;
}
