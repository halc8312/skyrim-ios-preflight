/* SPDX-License-Identifier: GPL-3.0-only */
#include "loading_menu.h"

enum {
    MENU_MANAGER = 0x012E3548, OPEN_CLOSE_DISPATCHER = 4, EVENT_OPENING = 4,
    UI_STRING_HOLDER = 0x012E35E0, LOADING_MENU_NAME = 0x74, CHARGEN_MENU_NAME = 0xB0
};

ClassicMenuTransition classic_observed_menu_transition(uint32_t site, uint32_t dispatcher, uint32_t event,
                                                       ClassicLoadingMenuReadWord read_word, void *guest) {
    const bool opening = site == CLASSIC_LOADING_MENU_OPEN_SITE;
    if ((!opening && site != CLASSIC_LOADING_MENU_CLOSE_SITE) || !read_word || !event || event > UINT32_MAX - 7)
        return CLASSIC_MENU_NONE;

    uint32_t manager;
    if (!read_word(guest, MENU_MANAGER, &manager) || !manager || manager > UINT32_MAX - OPEN_CLOSE_DISPATCHER ||
        dispatcher != manager + OPEN_CLOSE_DISPATCHER)
        return CLASSIC_MENU_NONE;

    uint32_t holder, actual_menu, event_opening;
    if (!read_word(guest, UI_STRING_HOLDER, &holder) || !holder || holder > UINT32_MAX - (CHARGEN_MENU_NAME + 3) ||
        !read_word(guest, event, &actual_menu) || !actual_menu ||
        !read_word(guest, event + EVENT_OPENING, &event_opening) || (uint8_t)event_opening != (uint8_t)opening)
        return CLASSIC_MENU_NONE;
    uint32_t name;
    if (read_word(guest, holder + LOADING_MENU_NAME, &name) && name == actual_menu)
        return opening ? CLASSIC_LOADING_MENU_OPEN : CLASSIC_LOADING_MENU_CLOSE;
    if (read_word(guest, holder + CHARGEN_MENU_NAME, &name) && name == actual_menu)
        return opening ? CLASSIC_CHARGEN_MENU_OPEN : CLASSIC_CHARGEN_MENU_CLOSE;
    return CLASSIC_MENU_NONE;
}
