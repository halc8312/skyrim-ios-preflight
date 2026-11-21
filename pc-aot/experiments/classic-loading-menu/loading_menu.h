/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    CLASSIC_LOADING_MENU_CLOSE_SITE = 0x00A5D6BC,
    CLASSIC_LOADING_MENU_OPEN_SITE = 0x00A5D9CF
};

typedef enum {
    CLASSIC_LOADING_MENU_NONE, CLASSIC_LOADING_MENU_OPEN, CLASSIC_LOADING_MENU_CLOSE
} ClassicLoadingMenuEvent;

typedef bool (*ClassicLoadingMenuReadWord)(void *guest, uint32_t address, uint32_t *value);

ClassicLoadingMenuEvent classic_loading_menu_event(uint32_t site, uint32_t dispatcher, uint32_t event,
                                                    ClassicLoadingMenuReadWord read_word, void *guest);
