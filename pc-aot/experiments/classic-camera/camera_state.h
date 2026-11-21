/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef bool (*ClassicCameraReadWord)(void *guest, uint32_t address, uint32_t *value);

enum { CLASSIC_CAMERA_SET_STATE = 0x006533D0 };
bool classic_camera_image_supported(const char *image_sha256);

/* Inspect camera state after the original guest function returns on the camera
 * thread. The caller supplies the verified guest image hash and a bounded
 * reader for little-endian guest words. Read failures, an unmatched camera or
 * state, or an unsupported image or entry return -1. */
int classic_camera_state_after_return(const char *image_sha256, uint32_t completed_entry,
                                      uint32_t receiver, ClassicCameraReadWord read_word, void *guest);
