/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_menu_focus_host.h"
#include "PcGuestMenuFocus.h"
#include "menu_focus.h"
#include "performance_metrics.h"
#include <stdatomic.h>

extern struct CPU *rt_current_cpu(void);

static ClassicMenuFocus state = CLASSIC_MENU_FOCUS_INIT;
static atomic_bool available, armed;

static void at_movie_advance(struct CPU *cpu, uint32_t menu, uint32_t view) {
    classic_menu_focus_advance(&state, cpu, rt_current_cpu(), menu, view, skyrim_perf_now_ns());
}

void skyrim_menu_focus_app_configure(bool requested) {
    bool possible = pc_guest_menu_focus_available();
    if (possible && requested) {
        possible = pc_guest_attach_menu_focus(at_movie_advance);
        if (possible) atomic_store_explicit(&armed, true, memory_order_release);
    }
    atomic_store_explicit(&available, possible, memory_order_release);
}

bool skyrim_menu_focus_app_available(void) {
    return atomic_load_explicit(&available, memory_order_acquire);
}

void skyrim_menu_focus_app_event(ClassicMenuIdentity identity, bool opening) {
    if (atomic_load_explicit(&armed, memory_order_acquire))
        classic_menu_focus_event(&state, identity, opening, rt_current_cpu());
}

uint64_t skyrim_menu_focus_touch_token(void) {
    return atomic_load_explicit(&armed, memory_order_acquire) ? classic_menu_focus_token(&state) : 0;
}

bool skyrim_menu_focus_touch_post(uint64_t token, double x, double y) {
    return token && classic_menu_focus_post(&state, token, x, y, skyrim_perf_now_ns());
}

void skyrim_menu_focus_touch_cancel(void) {
    if (atomic_load_explicit(&armed, memory_order_acquire)) classic_menu_focus_cancel(&state);
}
