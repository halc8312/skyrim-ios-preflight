/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Configured before the guest starts, only for detailed diagnostics. */
void skyrim_loading_menu_app_configure(const char *verified_image_sha256);
bool skyrim_loading_menu_app_ready(void);
void skyrim_loading_menu_app_observe(uint32_t site, uint32_t dispatcher, uint32_t event);
