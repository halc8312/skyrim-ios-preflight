/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    CLASSIC_LOADING_MENU_CLOSE_SITE = 0x00A5D6BC,
    CLASSIC_LOADING_MENU_OPEN_SITE = 0x00A5D9CF
};

/* Each kind has its own bit because menus (including confirmation dialogs) can nest. */
typedef enum {
    CLASSIC_MENU_NONE = 0, CLASSIC_MENU_LOADING = 1u, CLASSIC_MENU_CHARGEN = 2u,
    CLASSIC_MENU_INVENTORY = 1u << 2, CLASSIC_MENU_MAGIC = 1u << 3,
    CLASSIC_MENU_BARTER = 1u << 4, CLASSIC_MENU_GIFT = 1u << 5,
    CLASSIC_MENU_CONTAINER = 1u << 6, CLASSIC_MENU_CRAFTING = 1u << 7,
    CLASSIC_MENU_BOOK = 1u << 8, CLASSIC_MENU_DIALOGUE = 1u << 9, CLASSIC_MENU_TRAINING = 1u << 10,
    CLASSIC_MENU_JOURNAL = 1u << 11, CLASSIC_MENU_FAVORITES = 1u << 12,
    CLASSIC_MENU_MESSAGE = 1u << 13, CLASSIC_MENU_QUANTITY = 1u << 14,
    CLASSIC_MENU_CONSOLE = 1u << 15, CLASSIC_MENU_MAP = 1u << 16,
    CLASSIC_MENU_MAIN = 1u << 17
} ClassicMenu;

typedef bool (*ClassicLoadingMenuReadWord)(void *guest, uint32_t address, uint32_t *value);

ClassicMenu classic_dispatched_menu(uint32_t site, uint32_t dispatcher, uint32_t event,
                                    ClassicLoadingMenuReadWord read_word, void *guest);

typedef struct { ClassicMenu kind; uint32_t name; bool passive; } ClassicMenuIdentity;
ClassicMenuIdentity classic_dispatched_menu_identity(uint32_t site, uint32_t dispatcher, uint32_t event,
                                                    ClassicLoadingMenuReadWord read_word, void *guest);
