/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera_state.h"
#include <string.h>

static const char image_sha256_1932[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";

enum {
    PLAYER_CAMERA_SINGLETON = 0x012E7288,
    CURRENT_STATE_OFFSET = 0x20,
    PLAYER_STATE_TABLE_OFFSET = 0x6C,
    PLAYER_STATE_COUNT = 13,
    STATE_TABLE_LAST_BYTE = PLAYER_STATE_TABLE_OFFSET + 4 * PLAYER_STATE_COUNT - 1
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
