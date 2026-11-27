/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_menu_focus_host.h"
#include "PcGuestMenuFocus.h"
#include "menu_focus.h"
#include "performance_metrics.h"
#include <stdatomic.h>
#include <stdio.h>

extern struct CPU *rt_current_cpu(void);

static ClassicMenuFocus state = CLASSIC_MENU_FOCUS_INIT;
static atomic_bool available, tap_armed, viewport_armed;

static bool menu_observations_armed(void) {
    return atomic_load_explicit(&tap_armed, memory_order_acquire) ||
           atomic_load_explicit(&viewport_armed, memory_order_acquire);
}

static void at_movie_advance(struct CPU *cpu, uint32_t menu, uint32_t view) {
    const struct CPU *thread_cpu = rt_current_cpu();
    classic_menu_focus_advance(&state, cpu, thread_cpu, menu, view, skyrim_perf_now_ns());
    ClassicMenuViewport sample;
    if (atomic_load_explicit(&viewport_armed, memory_order_acquire) &&
        classic_menu_focus_viewport(&state, cpu, thread_cpu, menu, view, &sample)) {
        const int32_t *vp = sample.viewport, *vis = sample.visible, *safe = sample.safe;
        fprintf(stderr, "IOS_MENU_VIEWPORT schema=1 menu=%u mode=%u align=%u "
                        "viewport=%d,%d,%d,%d,%d,%d visible=%d,%d,%d,%d safe=%d,%d,%d,%d\n",
                sample.menu, sample.mode, sample.alignment, vp[0], vp[1], vp[2], vp[3], vp[4], vp[5],
                vis[0], vis[1], vis[2], vis[3], safe[0], safe[1], safe[2], safe[3]);
    }
}

void skyrim_menu_focus_app_configure(bool requested, bool viewport_diagnostics) {
    bool possible = pc_guest_menu_focus_available();
    if (possible && (requested || viewport_diagnostics)) {
        possible = pc_guest_attach_menu_focus(at_movie_advance);
        atomic_store_explicit(&tap_armed, possible && requested, memory_order_release);
        atomic_store_explicit(&viewport_armed, possible && viewport_diagnostics, memory_order_release);
    }
    atomic_store_explicit(&available, possible, memory_order_release);
}

bool skyrim_menu_focus_app_available(void) {
    return atomic_load_explicit(&available, memory_order_acquire);
}

void skyrim_menu_focus_app_event(ClassicMenuIdentity identity, bool opening) {
    if (menu_observations_armed())
        classic_menu_focus_event(&state, identity, opening, rt_current_cpu());
}

uint64_t skyrim_menu_focus_touch_token(void) {
    return atomic_load_explicit(&tap_armed, memory_order_acquire) ? classic_menu_focus_token(&state) : 0;
}

bool skyrim_menu_focus_touch_post(uint64_t token, double x, double y) {
    return token && atomic_load_explicit(&tap_armed, memory_order_acquire) &&
           classic_menu_focus_post(&state, token, x, y, skyrim_perf_now_ns());
}

void skyrim_menu_focus_touch_cancel(void) {
    if (menu_observations_armed()) classic_menu_focus_cancel(&state);
}
