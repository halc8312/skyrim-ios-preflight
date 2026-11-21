/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera_state.h"
#include <assert.h>
#include <stddef.h>

enum {
    CAMERA_GLOBAL = 0x12E7288, ACTOR_GLOBAL = 0x1B2E8E4,
    CAMERA = 0x10000, ACTOR = 0x11000, LOADED = 0x12000, FRAME = 0x13000,
    FIRST_LIST = FRAME + 0xC8, SECOND_LIST = FRAME + 0xD4,
    FIRST_NODES = 0x14000, SECOND_NODES = 0x15000,
    BODY = 0x16000, CHILD = 0x17000, GRANDCHILD = 0x18000, OTHER = 0x19000,
    UNOWNED = 0x1A000, ARMS = 0x1B000, SWITCHED_BODY = 0x1C000,
};

static const char image[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";
typedef struct {
    uint32_t camera_slot, first_count, first_capacity, second_count, body, arms;
    uint32_t child_parent, grandchild_parent;
    unsigned reads, root_reads, slots;
    bool replace_body;
} Scene;

static bool read_word(void *guest, uint32_t address, uint32_t *value) {
    Scene *scene = guest;
    ++scene->reads;
    switch (address) {
    case CAMERA_GLOBAL: *value = CAMERA; return true;
    case CAMERA + 0x20: *value = scene->camera_slot; return true;
    case CAMERA + 0x6C: *value = 0x20000; return true;
    case ACTOR_GLOBAL: *value = ACTOR; return true;
    case ACTOR + 0x44: *value = LOADED; return true;
    case LOADED + 0x20:
        *value = scene->root_reads++ && scene->replace_body ? SWITCHED_BODY : scene->body;
        return true;
    case ACTOR + 0x58C: *value = scene->arms; return true;
    case FIRST_LIST: *value = FIRST_NODES; return true;
    case FIRST_LIST + 4: *value = scene->first_capacity; return true;
    case FIRST_LIST + 8: *value = scene->first_count; return true;
    case SECOND_LIST: *value = SECOND_NODES; return true;
    case SECOND_LIST + 4: *value = 2; return true;
    case SECOND_LIST + 8: *value = scene->second_count; return true;
    case SECOND_NODES: *value = GRANDCHILD; return true;
    case SECOND_NODES + 4: *value = OTHER; return true;
    case BODY + 0x18: *value = UNOWNED; return true;
    case CHILD + 0x18: *value = scene->child_parent; return true;
    case GRANDCHILD + 0x18: *value = scene->grandchild_parent; return true;
    case OTHER + 0x18: *value = 0; return true;
    default:
        // Do not follow the unknown node or read undocumented fields.
        assert(address >= FIRST_NODES && address < FIRST_NODES + 128 * 4u);
        ++scene->slots;
        *value = address == FIRST_NODES ? BODY : address == FIRST_NODES + 4 ? CHILD : 0;
        return true;
    }
}

static ClassicCameraRenderSample sample(Scene *scene) {
    ClassicCameraRenderSample result;
    assert(classic_camera_sample_render(image, FRAME, FIRST_LIST, SECOND_LIST, read_word, scene, &result));
    return result;
}

int main(void) {
    Scene scene = {.camera_slot = 0x20000, .first_count = 2, .first_capacity = 2, .second_count = 2,
        .body = BODY, .arms = ARMS, .child_parent = BODY, .grandchild_parent = CHILD};
    ClassicCameraRenderSample result = sample(&scene);
    assert(result.first == (CLASSIC_CAMERA_RENDER_BODY | CLASSIC_CAMERA_RENDER_DESCENDANT));
    assert(result.second == (CLASSIC_CAMERA_RENDER_DESCENDANT | CLASSIC_CAMERA_RENDER_OTHER));

    scene.grandchild_parent = UNOWNED;
    result = sample(&scene);
    assert(result.second == (CLASSIC_CAMERA_RENDER_UNKNOWN | CLASSIC_CAMERA_RENDER_OTHER));

    scene.grandchild_parent = GRANDCHILD;
    result = sample(&scene);
    assert(result.second == (CLASSIC_CAMERA_RENDER_UNKNOWN | CLASSIC_CAMERA_RENDER_OTHER));

    scene.first_count = 133;
    result = sample(&scene);
    assert(result.first == CLASSIC_CAMERA_RENDER_UNKNOWN);

    scene.first_capacity = 133;
    scene.slots = 0;
    result = sample(&scene);
    assert(scene.slots == 128);
    assert(result.first == (CLASSIC_CAMERA_RENDER_BODY | CLASSIC_CAMERA_RENDER_DESCENDANT |
                            CLASSIC_CAMERA_RENDER_UNKNOWN));

    scene.first_count = scene.second_count = 0;
    result = sample(&scene);
    assert(result.first == 0 && result.second == 0);

    scene.arms = BODY;
    result = sample(&scene);
    assert(result.first == CLASSIC_CAMERA_RENDER_UNKNOWN && result.second == CLASSIC_CAMERA_RENDER_UNKNOWN);

    scene.arms = ARMS;
    scene.replace_body = true;
    scene.root_reads = 0;
    assert(!classic_camera_sample_render(image, FRAME, FIRST_LIST, SECOND_LIST, read_word, &scene, &result));

    scene.camera_slot = 0x21000;
    assert(!classic_camera_sample_render(image, FRAME, FIRST_LIST, SECOND_LIST, read_word, &scene, &result));
    scene.reads = 0;
    assert(!classic_camera_sample_render(image, FRAME, SECOND_LIST, FIRST_LIST, read_word, &scene, &result));
    assert(!classic_camera_sample_render("wrong", FRAME, FIRST_LIST, SECOND_LIST, read_word, &scene, &result));
    assert(!scene.reads);
    return 0;
}
