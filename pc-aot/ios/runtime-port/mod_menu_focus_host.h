/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "loading_menu.h"

/* Called once after the optional menu observer was selected, before the guest starts. */
void skyrim_menu_focus_app_configure(bool requested, bool viewport_diagnostics);
bool skyrim_menu_focus_app_available(void);
void skyrim_menu_focus_app_event(ClassicMenuIdentity identity, bool opening);
uint64_t skyrim_menu_focus_touch_token(void);
bool skyrim_menu_focus_touch_post(uint64_t token, double x, double y);
void skyrim_menu_focus_touch_cancel(void);
