/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_camera_host.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

uint8_t *MEM;

enum { SINGLETON = 0x012E7288, PLAYER = 0x20000, CURRENT = PLAYER + 0x20, TABLE = PLAYER + 0x6C };

static void word(uint32_t address, uint32_t value) { memcpy(MEM + address, &value, sizeof value); }

int main(void) {
    assert(!skyrim_camera_app_ready());
    skyrim_camera_app_configure("unsupported");
    assert(!skyrim_camera_app_ready());
    skyrim_camera_app_configure("d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605");
    assert(skyrim_camera_app_ready());
    skyrim_camera_app_observe(PLAYER); /* Memory has not been bound yet. */

    MEM = calloc(1, SINGLETON + 4);
    assert(MEM);
    word(SINGLETON, 0x8000);
    skyrim_camera_app_observe(0x8000);
    word(SINGLETON, PLAYER);
    word(TABLE, 0x30000);
    word(TABLE + 12 * 4, 0x30010);

    word(CURRENT, 0x30000);
    skyrim_camera_app_observe(PLAYER + 4);
    skyrim_camera_app_observe(PLAYER);
    skyrim_camera_app_observe(PLAYER); /* Same slot: no second line. */
    word(CURRENT, 0);
    skyrim_camera_app_observe(PLAYER);
    word(CURRENT, 0x30010);
    skyrim_camera_app_observe(PLAYER);
    uint32_t after;
    memcpy(&after, MEM + CURRENT, sizeof after);
    assert(after == 0x30010);
    free(MEM);
    return 0;
}
