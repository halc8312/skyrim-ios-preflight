/* SPDX-License-Identifier: GPL-3.0-only */
/* Authored caps and refresh rates only. */
#include "present_pacing_core.h"
#include "mobile_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define MS UINT64_C(1000000)

static void min_duration(void) {
    assert(skyrim_present_min_duration_ns(0) == 0);          /* no cap: ordinary present */
    assert(skyrim_present_min_duration_ns(60) == 16666666);
    assert(skyrim_present_min_duration_ns(40) == 25 * MS);
    assert(skyrim_present_min_duration_ns(30) == 33333333);
    assert(skyrim_present_min_duration_ns(24) == 41666666);
    assert(skyrim_present_min_duration_ns(240) == 4166666);
    assert(skyrim_present_min_duration_ns(241) == 0);        /* same bound as the CPU pacer */
    assert(skyrim_present_min_duration_ns(~0u) == 0);
}

static void cadence_and_refresh_request(void) {
    /* Even: every frame stays up for the same number of refreshes. */
    const unsigned even[][2] = {{60, 60}, {60, 30}, {120, 60}, {120, 40}, {120, 30}, {120, 24}, {120, 120}};
    for (unsigned i = 0; i < sizeof even / sizeof even[0]; ++i) {
        unsigned lo = 0, hi = 0, want = 0;
        assert(skyrim_present_even_cadence(even[i][0], even[i][1]));
        assert(skyrim_present_refresh_request(even[i][0], even[i][1], &lo, &hi, &want));
        assert(lo == even[i][1] && hi == even[i][0] && want == even[i][0]);
    }
    /* Uneven: 24 and 40 on a 60 Hz panel, and anything the panel cannot reach. */
    const unsigned uneven[][2] = {{60, 24}, {60, 40}, {60, 45}, {120, 50}, {120, 90}, {60, 120}, {0, 30}, {120, 0}};
    for (unsigned i = 0; i < sizeof uneven / sizeof uneven[0]; ++i) {
        unsigned lo = 7, hi = 7, want = 7;
        assert(!skyrim_present_even_cadence(uneven[i][0], uneven[i][1]));
        assert(!skyrim_present_refresh_request(uneven[i][0], uneven[i][1], &lo, &hi, &want));
        assert(lo == 7 && hi == 7 && want == 7);            /* outputs untouched */
    }
}

static void cpu_deadline(void) {
    const uint64_t period = 16666666;
    /* Off: exactly the established deadline, whatever the inputs. */
    assert(skyrim_present_cpu_deadline(1000 * MS, 990 * MS, period, 0) == 1000 * MS);
    assert(skyrim_present_cpu_deadline(1000 * MS, 1100 * MS, period, 0) == 1000 * MS);
    assert(skyrim_present_cpu_deadline(5, 0, 0, 0) == 5);
    /* On: half a period earlier, never in the past, and unchanged without a period. */
    assert(skyrim_present_cpu_deadline(1000 * MS, 990 * MS, period, 1) == 1000 * MS - period / 2);
    assert(skyrim_present_cpu_deadline(1000 * MS, 995 * MS, period, 1) == 995 * MS);
    assert(skyrim_present_cpu_deadline(1000 * MS, 999 * MS, period, 1) == 999 * MS);
    assert(skyrim_present_cpu_deadline(1000 * MS, 1000 * MS, period, 1) == 1000 * MS);
    assert(skyrim_present_cpu_deadline(1000 * MS, 1500 * MS, period, 1) == 1500 * MS);
    assert(skyrim_present_cpu_deadline(4 * MS, 0, period, 1) == 4 * MS);      /* too early to subtract */
    assert(skyrim_present_cpu_deadline(1000 * MS, 0, 0, 1) == 1000 * MS);

    /* The real pacer clock feeds this: a steady cap keeps its schedule, a late
     * frame resets it, and the early release never overshoots a deadline. */
    SkyrimFrameClock clock = {0, 0};
    uint64_t now = 10 * 1000 * MS;
    assert(skyrim_mobile_frame_deadline(&clock, now, 60) == now);             /* first frame: no wait */
    for (unsigned frame = 1; frame < 50; ++frame) {
        now += period / 2;                                                      /* the game was fast */
        const uint64_t deadline = skyrim_mobile_frame_deadline(&clock, now, 60);
        const uint64_t early = skyrim_present_cpu_deadline(deadline, now, period, 1);
        assert(early <= deadline && early >= now);
        assert(deadline - early <= period / 2);
        now = early;                                                            /* slept to the early deadline */
    }
}

static void target_and_setting(void) {
    assert(skyrim_present_target() == 0);
    skyrim_present_set_target(skyrim_present_min_duration_ns(60));
    assert(skyrim_present_target() == 16666666);
    skyrim_present_set_target(0);
    assert(skyrim_present_target() == 0);
    /* Only exactly "1" enables it; the value is read once. */
    assert(!skyrim_present_sync_enabled());
    setenv("SKYRIM_IOS_DISPLAY_SYNC", "1", 1);
    assert(!skyrim_present_sync_enabled());   /* cached by the first read */
}

int main(int argc, char **argv) {
    min_duration();
    cadence_and_refresh_request();
    cpu_deadline();
    if (argc > 1 && argv[1][0] == '1') {
        /* Run with the environment already set: enabled. */
        assert(skyrim_present_sync_enabled());
        assert(skyrim_present_sync_enabled());
    } else {
        target_and_setting();
    }
    puts("present_pacing_core=PASS");
    return 0;
}
