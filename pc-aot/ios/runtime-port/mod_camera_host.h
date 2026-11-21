/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Call once at startup with the verified, pinned guest image and detailed
 * diagnostics enabled. The hook is read-only and runs on the camera thread. */
void skyrim_camera_app_configure(const char *verified_image_sha256);
bool skyrim_camera_app_ready(void);
void skyrim_camera_app_observe(uint32_t receiver);
