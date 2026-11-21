/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_loading_menu_host.h"
#include "device_policy.h"
#include "loading_menu.h"
#include "performance_metrics.h"

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

extern uint8_t *MEM;

enum { LOADING = 1u, CHARGEN = 2u };
static bool ready, verbose;
static atomic_uint open_menus;

void skyrim_loading_menu_app_configure(const char *verified_image_sha256, bool diagnostics) {
    static const char classic_1932[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";
    ready = verified_image_sha256 && strcmp(verified_image_sha256, classic_1932) == 0;
    verbose = diagnostics && ready;
    atomic_store_explicit(&open_menus, 0, memory_order_relaxed);
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
    const ClassicMenuTransition direction = classic_observed_menu_transition(site, dispatcher, event, read_menu_word, MEM);
    if (direction == CLASSIC_MENU_NONE) return;
    const bool chargen = direction == CLASSIC_CHARGEN_MENU_OPEN || direction == CLASSIC_CHARGEN_MENU_CLOSE;
    const bool opening = direction == CLASSIC_LOADING_MENU_OPEN || direction == CLASSIC_CHARGEN_MENU_OPEN;
    const unsigned menu = chargen ? CHARGEN : LOADING;
    if (opening) atomic_fetch_or_explicit(&open_menus, menu, memory_order_relaxed);
    else atomic_fetch_and_explicit(&open_menus, ~menu, memory_order_relaxed);
    if (verbose)
        fprintf(stderr, "%s opening=%d ts_ns=%llu\n", chargen ? "IOS_CHARGEN_MENU" : "IOS_LOADING_MENU",
                opening, (unsigned long long)skyrim_perf_now_ns());
}

bool skyrim_safety_load_app_bypass(void) {
    return ready && atomic_load_explicit(&open_menus, memory_order_relaxed) == LOADING &&
           !skyrim_device_memory_pressure(skyrim_perf_now_ns());
}
