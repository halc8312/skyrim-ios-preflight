/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Configured before the guest starts. Only detailed diagnostics log events. */
void skyrim_loading_menu_app_configure(const char *verified_image_sha256, bool diagnostics);
bool skyrim_loading_menu_app_ready(void);
void skyrim_loading_menu_app_observe(uint32_t site, uint32_t dispatcher, uint32_t event);
typedef struct {
    uint32_t menus; /* Zero outside the supported touch menus. */
    bool list_navigation;
} SkyrimTouchMenu;
SkyrimTouchMenu skyrim_loading_menu_touch_state(void);
bool skyrim_safety_load_app_bypass(void);
