/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_camera_host.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

uint8_t *MEM;

enum {
    SINGLETON = 0x012E7288, PLAYER = 0x20000, CURRENT = PLAYER + 0x20, TABLE = PLAYER + 0x6C,
    ACTOR_SINGLETON = 0x01B2E8E4, ACTOR = 0x40000, LOADED_STATE = 0x41000,
    ARMS_ROOT = 0x42000, BODY_ROOT = 0x43000, PARENT = 0x44000,
    FRAME = 0x45000, FIRST = FRAME + 0xC8, SECOND = FRAME + 0xD4, ITEMS = 0x46000
};

static void word(uint32_t address, uint32_t value) { memcpy(MEM + address, &value, sizeof value); }

int main(void) {
    assert(!skyrim_camera_app_ready());
    skyrim_camera_app_configure("unsupported");
    assert(!skyrim_camera_app_ready());
    skyrim_camera_app_configure("d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605");
    assert(skyrim_camera_app_ready());
    skyrim_camera_app_observe(PLAYER); /* Memory has not been bound yet. */

    MEM = calloc(1, ACTOR_SINGLETON + 4);
    assert(MEM);
    word(SINGLETON, 0x8000);
    skyrim_camera_app_observe(0x8000);
    word(SINGLETON, PLAYER);
    word(TABLE, 0x30000);
    word(TABLE + 12 * 4, 0x30010);
    word(ACTOR_SINGLETON, ACTOR);
    word(ACTOR + 0x58C, ARMS_ROOT);
    word(ACTOR + 0x44, LOADED_STATE);
    word(LOADED_STATE + 0x20, BODY_ROOT);
    word(ARMS_ROOT + 0x18, PARENT);
    word(BODY_ROOT + 0x18, PARENT);
    word(BODY_ROOT + 0x98, 0x101);

    word(CURRENT, 0x30000);
    skyrim_camera_app_observe(PLAYER + 4);
    skyrim_camera_app_observe(PLAYER);
    skyrim_camera_app_observe(PLAYER); /* Same state: no second line. */
    word(BODY_ROOT + 0x98, 0x100);
    skyrim_camera_app_observe(PLAYER); /* Root changed without a slot change. */
    skyrim_camera_app_observe(PLAYER);
    word(FIRST, ITEMS);
    word(FIRST + 4, 1);
    word(FIRST + 8, 1);
    word(ITEMS, BODY_ROOT);
    skyrim_camera_app_observe_render(FRAME, FIRST, SECOND);
    for (unsigned i = 0; i < 120; ++i) skyrim_camera_app_observe_render(FRAME, FIRST, SECOND);
    word(FIRST + 8, 0);
    word(SECOND, ITEMS);
    word(SECOND + 4, 1);
    word(SECOND + 8, 1);
    word(ITEMS, ARMS_ROOT);
    word(ARMS_ROOT + 0x18, BODY_ROOT);
    for (unsigned i = 0; i < 120; ++i) skyrim_camera_app_observe_render(FRAME, FIRST, SECOND);
    word(CURRENT, 0);
    skyrim_camera_app_observe(PLAYER);
    word(CURRENT, 0x30010);
    skyrim_camera_app_observe(PLAYER);
    uint32_t after;
    memcpy(&after, MEM + CURRENT, sizeof after);
    assert(after == 0x30010);
    memcpy(&after, MEM + BODY_ROOT + 0x98, sizeof after);
    assert(after == 0x100);
    free(MEM);
    return 0;
}
