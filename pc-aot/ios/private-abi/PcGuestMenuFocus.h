/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct CPU;
typedef void (*PcGuestMenuFocusAdvance)(struct CPU *, uint32_t menu, uint32_t view);
typedef bool (*PcGuestBindMenuFocus)(uint32_t version, PcGuestMenuFocusAdvance callback);
#define PC_GUEST_MENU_FOCUS_VERSION 1u
#define PC_GUEST_MENU_FOCUS_BIND "skyrim_guest_bind_menu_focus_v1"

/* Optional capability of newer guests with the Classic menu observer. */
bool pc_guest_menu_focus_available(void);
bool pc_guest_attach_menu_focus(PcGuestMenuFocusAdvance callback);
