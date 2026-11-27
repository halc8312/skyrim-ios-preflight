/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_loading_menu_host.h"
#include "device_policy.h"
#include "loading_menu.h"
#ifdef SKYRIM_IOS_MOD_HOOKS
#include "mod_menu_focus_host.h"
#endif
#include "performance_metrics.h"

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

extern uint8_t *MEM;

static bool ready, verbose;
static atomic_uint open_menus;
static atomic_ullong safety_accepted;

void skyrim_loading_menu_app_configure(const char *verified_image_sha256, bool diagnostics) {
    static const char classic_1932[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";
    ready = verified_image_sha256 && strcmp(verified_image_sha256, classic_1932) == 0;
    verbose = diagnostics && ready;
    atomic_store_explicit(&open_menus, 0, memory_order_relaxed);
    atomic_store_explicit(&safety_accepted, 0, memory_order_relaxed);
}

bool skyrim_loading_menu_app_ready(void) { return ready; }

static bool read_menu_word(void *memory, uint32_t address, uint32_t *value) {
    /* The guest mapping reserves 4 GiB and protects the first 64 KiB. */
    if (address < 0x10000 || address > UINT32_MAX - (sizeof(*value) - 1)) return false;
    memcpy(value, (const uint8_t *)memory + address, sizeof(*value));
    return true;
}

void skyrim_loading_menu_app_observe(uint32_t site, uint32_t dispatcher, uint32_t event) {
    if (!ready || !MEM) return;
    const bool opening = site == CLASSIC_LOADING_MENU_OPEN_SITE;
    const ClassicMenuIdentity identity = classic_dispatched_menu_identity(site, dispatcher, event, read_menu_word, MEM);
#ifdef SKYRIM_IOS_MOD_HOOKS
    if (identity.name) skyrim_menu_focus_app_event(identity, opening);
#endif
    const ClassicMenu menu = identity.kind;
    if (menu == CLASSIC_MENU_NONE) return;
    if (opening) atomic_fetch_or_explicit(&open_menus, menu, memory_order_relaxed);
    else atomic_fetch_and_explicit(&open_menus, ~menu, memory_order_relaxed);
    if (verbose && (menu == CLASSIC_MENU_CHARGEN || menu == CLASSIC_MENU_LOADING)) {
        fprintf(stderr, "%s opening=%d ts_ns=%llu\n", menu == CLASSIC_MENU_CHARGEN ? "IOS_CHARGEN_MENU" : "IOS_LOADING_MENU",
                opening, (unsigned long long)skyrim_perf_now_ns());
        if (menu == CLASSIC_MENU_LOADING && !opening) {
            const unsigned long long accepted = atomic_exchange_explicit(&safety_accepted, 0, memory_order_relaxed);
            if (accepted) fprintf(stderr, "IOS_SAFETY_LOAD accepted=%llu\n", accepted);
        }
    }
}

SkyrimTouchMenu skyrim_loading_menu_touch_state(void) {
    const unsigned open = atomic_load_explicit(&open_menus, memory_order_relaxed);
    const unsigned lists = CLASSIC_MENU_INVENTORY | CLASSIC_MENU_MAGIC | CLASSIC_MENU_CONTAINER |
                           CLASSIC_MENU_BARTER | CLASSIC_MENU_GIFT | CLASSIC_MENU_CRAFTING;
    const unsigned pages = CLASSIC_MENU_BOOK | CLASSIC_MENU_DIALOGUE | CLASSIC_MENU_TRAINING |
                           CLASSIC_MENU_JOURNAL | CLASSIC_MENU_FAVORITES;
    const unsigned modal = CLASSIC_MENU_MESSAGE | CLASSIC_MENU_QUANTITY;
    /* The radial menu can remain open underneath its inventory or magic submenu.
     * The map and character preview still need their analog controls. */
    if (!(open & (lists | pages)) || (open & (CLASSIC_MENU_LOADING | CLASSIC_MENU_CHARGEN |
                                              CLASSIC_MENU_CONSOLE | CLASSIC_MENU_MAIN | CLASSIC_MENU_MAP)))
        return (SkyrimTouchMenu){0};
    const unsigned visible = open & (lists | pages | modal);
    return (SkyrimTouchMenu){ visible, (visible & lists) && !(visible & (visible - 1u)) };
}

bool skyrim_safety_load_app_bypass(void) {
    unsigned guard = atomic_load_explicit(&open_menus, memory_order_relaxed) & (CLASSIC_MENU_LOADING | CLASSIC_MENU_CHARGEN);
    if (!ready || guard != CLASSIC_MENU_LOADING ||
        skyrim_device_memory_pressure(skyrim_perf_now_ns())) return false;
    if (verbose) atomic_fetch_add_explicit(&safety_accepted, 1, memory_order_relaxed);
    return true;
}
