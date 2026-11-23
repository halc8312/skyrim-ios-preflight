/* SPDX-License-Identifier: GPL-3.0-only */
#include "present_pacing_core.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

uint64_t skyrim_present_min_duration_ns(unsigned fps_limit) {
    if (!fps_limit || fps_limit > 240) return 0;
    return UINT64_C(1000000000) / fps_limit;
}

int skyrim_present_even_cadence(unsigned display_hz, unsigned fps_limit) {
    if (!display_hz || !fps_limit || fps_limit > display_hz) return 0;
    return display_hz % fps_limit == 0;
}

int skyrim_present_refresh_request(unsigned display_hz, unsigned fps_limit,
                                   unsigned *minimum, unsigned *maximum, unsigned *preferred) {
    /* Only a refresh that is an integer multiple of the cap keeps every frame on
     * screen for the same number of refreshes. The panel's own rate is the
     * highest such multiple; it also leaves the most slack for a late frame. */
    if (!skyrim_present_even_cadence(display_hz, fps_limit)) return 0;
    *minimum = fps_limit;
    *maximum = display_hz;
    *preferred = display_hz;
    return 1;
}

uint64_t skyrim_present_cpu_deadline(uint64_t deadline, uint64_t now, uint64_t period_ns, int sync) {
    if (!sync || !period_ns) return deadline;
    const uint64_t early = deadline > period_ns / 2 ? deadline - period_ns / 2 : deadline;
    return early > now ? early : now;
}

static _Atomic uint64_t target_ns;
void skyrim_present_set_target(uint64_t min_duration_ns) {
    atomic_store_explicit(&target_ns, min_duration_ns, memory_order_relaxed);
}
uint64_t skyrim_present_target(void) {
    return atomic_load_explicit(&target_ns, memory_order_relaxed);
}

int skyrim_present_sync_enabled(void) {
    static int cached = -1;
    int value = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (value < 0) {
        const char *setting = getenv("SKYRIM_IOS_DISPLAY_SYNC");
        value = setting && !strcmp(setting, "1");
        __atomic_store_n(&cached, value, __ATOMIC_RELAXED);
    }
    return value;
}
