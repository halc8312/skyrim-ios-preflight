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

enum { RENDER_LIST_LIMIT = 128, RENDER_PARENT_LIMIT = 8 };

typedef struct {
    uint32_t nodes[RENDER_LIST_LIMIT];
    uint32_t parents[RENDER_LIST_LIMIT];
    unsigned count, mask;
} CameraRenderList;

static bool first_person(ClassicCameraReadWord read_word, void *guest) {
    uint32_t camera, current, first;
    return read_word(guest, PLAYER_CAMERA_SINGLETON, &camera) &&
           read_field(read_word, guest, camera, CURRENT_STATE_OFFSET, &current) && current &&
           read_field(read_word, guest, camera, PLAYER_STATE_TABLE_OFFSET, &first) && current == first;
}

static bool body_identity(ClassicCameraReadWord read_word, void *guest, uint32_t *identity) {
    uint32_t actor, loaded, arms;
    return read_word(guest, PLAYER_ACTOR_SINGLETON, &actor) &&
           read_field(read_word, guest, actor, ACTOR_LOADED_STATE, &loaded) &&
           read_field(read_word, guest, loaded, LOADED_STATE_ROOT, identity) && valid_object_address(*identity) &&
           read_field(read_word, guest, actor, ACTOR_FIRST_PERSON_ROOT, &arms) && arms != *identity;
}

static void capture_list(ClassicCameraReadWord read_word, void *guest, uint32_t list, CameraRenderList *out) {
    uint32_t nodes, capacity, count;
    if (!read_field(read_word, guest, list, 0x8, &count) ||
        !read_field(read_word, guest, list, 0x4, &capacity) || count > capacity ||
        (count && (!read_field(read_word, guest, list, 0, &nodes) || !valid_object_address(nodes) ||
                   count - 1 > (UINT32_MAX - nodes) / 4))) {
        out->mask = CLASSIC_CAMERA_RENDER_UNKNOWN;
        return;
    }
    if (count > RENDER_LIST_LIMIT) {
        count = RENDER_LIST_LIMIT;
        out->mask = CLASSIC_CAMERA_RENDER_UNKNOWN;
    }
    for (unsigned i = 0; i < count; ++i) {
        uint32_t node;
        if (!read_field(read_word, guest, nodes, i * 4u, &node)) {
            out->mask |= CLASSIC_CAMERA_RENDER_UNKNOWN;
            break;
        }
        if (!node) continue;
        uint32_t parent;
        // Membership is proven by the native frame's retained list. The parent
        // value is only used as an identity and is not dereferenced on its own.
        if (!read_field(read_word, guest, node, SCENE_ROOT_PARENT, &parent)) {
            out->mask |= CLASSIC_CAMERA_RENDER_UNKNOWN;
            continue;
        }
        out->nodes[out->count] = node;
        out->parents[out->count++] = parent;
    }
}

static uint32_t retained_parent(const CameraRenderList lists[2], uint32_t node) {
    for (unsigned list = 0; list < 2; ++list) {
        for (unsigned i = 0; i < lists[list].count; ++i) {
            if (lists[list].nodes[i] == node) return lists[list].parents[i];
        }
    }
    return UINT32_MAX;
}

static unsigned relation(const CameraRenderList lists[2], uint32_t body, uint32_t node, uint32_t parent) {
    if (node == body) return CLASSIC_CAMERA_RENDER_BODY;
    for (unsigned depth = 0; depth < RENDER_PARENT_LIMIT; ++depth) {
        if (!parent) return CLASSIC_CAMERA_RENDER_OTHER;
        if (parent == body) return CLASSIC_CAMERA_RENDER_DESCENDANT;
        if (!valid_object_address(parent)) break;
        parent = retained_parent(lists, parent);
    }
    return CLASSIC_CAMERA_RENDER_UNKNOWN;
}

bool classic_camera_sample_render(const char *image_sha256, uint32_t frame, uint32_t first, uint32_t second,
                                  ClassicCameraReadWord read_word, void *guest, ClassicCameraRenderSample *sample) {
    if (!sample || !read_word || !classic_camera_image_supported(image_sha256) ||
        !valid_object_address(frame) || frame > UINT32_MAX - 0xDF ||
        first != frame + 0xC8 || second != frame + 0xD4 || !first_person(read_word, guest))
        return false;

    uint32_t body = 0, after = 0;
    if (!body_identity(read_word, guest, &body)) {
        *sample = (ClassicCameraRenderSample){CLASSIC_CAMERA_RENDER_UNKNOWN, CLASSIC_CAMERA_RENDER_UNKNOWN};
        return true;
    }
    CameraRenderList lists[2] = {0};
    capture_list(read_word, guest, first, &lists[0]);
    capture_list(read_word, guest, second, &lists[1]);
    for (unsigned list = 0; list < 2; ++list) {
        CameraRenderList *current = &lists[list];
        for (unsigned i = 0; i < current->count; ++i)
            current->mask |= relation(lists, body, current->nodes[i], current->parents[i]);
    }
    if (!first_person(read_word, guest) || !body_identity(read_word, guest, &after) || after != body)
        return false;
    *sample = (ClassicCameraRenderSample){lists[0].mask, lists[1].mask};
    return true;
}

#ifdef SKYRIM_CLASSIC_CAMERA_CULL_TRIAL
#include "camera_cull_trial.h"

bool classic_camera_cull_identity(ClassicCameraReadWord read_word, void *guest, ClassicCameraCullIdentity *identity) {
    uint32_t current, arms;
    return read_word(guest, PLAYER_CAMERA_SINGLETON, &identity->camera) &&
           read_field(read_word, guest, identity->camera, CURRENT_STATE_OFFSET, &current) && current &&
           read_field(read_word, guest, identity->camera, PLAYER_STATE_TABLE_OFFSET, &identity->first_state) &&
           current == identity->first_state &&
           read_word(guest, PLAYER_ACTOR_SINGLETON, &identity->actor) &&
           read_field(read_word, guest, identity->actor, ACTOR_LOADED_STATE, &identity->loaded) &&
           read_field(read_word, guest, identity->loaded, LOADED_STATE_ROOT, &identity->body) &&
           valid_object_address(identity->body) &&
           read_field(read_word, guest, identity->actor, ACTOR_FIRST_PERSON_ROOT, &arms) &&
           arms != identity->body;
}
#endif
