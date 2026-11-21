/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_loading_menu_host.h"
#include "loading_menu.h"
#include "performance_metrics.h"

#include <stdio.h>
#include <string.h>

extern uint8_t *MEM;

static bool ready;

void skyrim_loading_menu_app_configure(const char *verified_image_sha256) {
    static const char classic_1932[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";
    ready = verified_image_sha256 && strcmp(verified_image_sha256, classic_1932) == 0;
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
    fprintf(stderr, "%s opening=%d ts_ns=%llu\n", chargen ? "IOS_CHARGEN_MENU" : "IOS_LOADING_MENU",
            opening, (unsigned long long)skyrim_perf_now_ns());
}
