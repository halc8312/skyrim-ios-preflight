/* SPDX-License-Identifier: GPL-3.0-only */
#include "loading_menu.h"

enum {
    MENU_MANAGER = 0x012E3548, OPEN_CLOSE_DISPATCHER = 4, EVENT_OPENING = 4,
    UI_STRING_HOLDER = 0x012E35E0, LAST_MENU_NAME = 0xBC
};

/* Interned names in LE 1.9.32 UIStringHolder (Classic SKSE 1.7.3 GameMenus.h).
 * Compare pointers at the original dispatch rather than scanning the guest on the UIKit thread. */
static const struct { uint8_t offset; ClassicMenu kind; } menus[] = {
    {0x74, CLASSIC_MENU_LOADING}, {0xB0, CLASSIC_MENU_CHARGEN},
    {0x44, CLASSIC_MENU_INVENTORY}, {0x64, CLASSIC_MENU_MAGIC},
    {0x7C, CLASSIC_MENU_BARTER}, {0x80, CLASSIC_MENU_GIFT},
    {0x98, CLASSIC_MENU_CONTAINER}, {0xB4, CLASSIC_MENU_CRAFTING},
    {0xA8, CLASSIC_MENU_BOOK}, {0x4C, CLASSIC_MENU_DIALOGUE}, {0xB8, CLASSIC_MENU_TRAINING},
    {0xA4, CLASSIC_MENU_JOURNAL}, {0xAC, CLASSIC_MENU_FAVORITES},
    {0x58, CLASSIC_MENU_MESSAGE}, {0x90, CLASSIC_MENU_QUANTITY},
    {0x48, CLASSIC_MENU_CONSOLE}, {0x88, CLASSIC_MENU_MAP},
    {0x54, CLASSIC_MENU_MAIN}
};

ClassicMenuIdentity classic_dispatched_menu_identity(uint32_t site, uint32_t dispatcher, uint32_t event,
                                                    ClassicLoadingMenuReadWord read_word, void *guest) {
    const ClassicMenuIdentity none = {0};
    const bool opening = site == CLASSIC_LOADING_MENU_OPEN_SITE;
    if ((!opening && site != CLASSIC_LOADING_MENU_CLOSE_SITE) || !read_word || !event || event > UINT32_MAX - 7)
        return none;

    uint32_t manager;
    if (!read_word(guest, MENU_MANAGER, &manager) || !manager || manager > UINT32_MAX - OPEN_CLOSE_DISPATCHER ||
        dispatcher != manager + OPEN_CLOSE_DISPATCHER)
        return none;

    uint32_t holder, actual_menu, event_opening;
    if (!read_word(guest, UI_STRING_HOLDER, &holder) || !holder || holder > UINT32_MAX - (LAST_MENU_NAME + 3) ||
        !read_word(guest, event, &actual_menu) || !actual_menu ||
        !read_word(guest, event + EVENT_OPENING, &event_opening) || (uint8_t)event_opening != (uint8_t)opening)
        return none;
    for (unsigned i = 0; i < sizeof(menus) / sizeof(menus[0]); ++i) {
        uint32_t name;
        if (read_word(guest, holder + menus[i].offset, &name) && name == actual_menu)
            return (ClassicMenuIdentity){menus[i].kind, actual_menu, false};
    }
    /* The HUD, cursor and radial parent can stay open behind a list. The original
     * Fader (0x85B110) and Mist (0x87B440) pass mouse moves to the next menu. */
    static const unsigned passive_slots[] = {0x50, 0x5c, 0x60, 0x78, 0xbc};
    for (unsigned i = 0; i < sizeof(passive_slots) / sizeof(passive_slots[0]); ++i) {
        uint32_t name;
        if (read_word(guest, holder + passive_slots[i], &name) && name == actual_menu)
            return (ClassicMenuIdentity){CLASSIC_MENU_NONE, actual_menu, true};
    }
    return (ClassicMenuIdentity){CLASSIC_MENU_NONE, actual_menu, false};
}

ClassicMenu classic_dispatched_menu(uint32_t site, uint32_t dispatcher, uint32_t event,
                                    ClassicLoadingMenuReadWord read_word, void *guest) {
    return classic_dispatched_menu_identity(site, dispatcher, event, read_word, guest).kind;
}
