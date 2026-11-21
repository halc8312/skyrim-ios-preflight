/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_camera_host.h"
#include "camera_state.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern uint8_t *MEM;

static char image_sha256[65];
static atomic_int last_sample = -1;

void skyrim_camera_app_configure(const char *verified_image_sha256) {
    if (!image_sha256[0] && classic_camera_image_supported(verified_image_sha256))
        memcpy(image_sha256, verified_image_sha256, sizeof image_sha256);
}

bool skyrim_camera_app_ready(void) { return image_sha256[0] != 0; }

static bool read_camera_word(void *memory, uint32_t address, uint32_t *value) {
    /* The guest reserves 4 GiB, with its first 64 KiB inaccessible. */
    if (!memory || address < 0x10000 || address > UINT32_MAX - (sizeof(*value) - 1)) return false;
    memcpy(value, (const uint8_t *)memory + address, sizeof(*value));
    return true;
}

void skyrim_camera_app_observe(uint32_t receiver) {
    ClassicCameraSample sample;
    if (!classic_camera_sample_after_return(image_sha256, CLASSIC_CAMERA_SET_STATE, receiver, read_camera_word,
                                            MEM, &sample))
        return;
    // Pack the slot and the two small classifications for log de-duplication.
    const int key = (sample.slot << 8) | (sample.arms << 4) | sample.body;
    const int previous = atomic_exchange_explicit(&last_sample, key, memory_order_relaxed);
    if (previous < 0 || (previous >> 8) != sample.slot)
        fprintf(stderr, "IOS_CAMERA_STATE slot=%d\n", sample.slot);
    if (sample.sampled_roots && previous != key)
        fprintf(stderr, "IOS_CAMERA_BODY schema=1 slot=%d arms=%d body=%d\n", sample.slot, sample.arms, sample.body);
}
