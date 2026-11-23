/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* "Present in step with the display" decisions: portable C, no Apple headers.
 *
 * Opt-in (default off, SKYRIM_IOS_DISPLAY_SYNC=1 from the startup switch). The
 * frame cap stays the user's; only how it is kept changes. Instead of sleeping
 * on the CPU clock and handing Metal a frame at an arbitrary moment, the frame
 * limit is given to Metal as a minimum on-screen duration, so the display
 * compositor spaces frames. The CPU pacer then only has to keep the game from
 * running far ahead, so it releases a little early. Nothing here changes what
 * is drawn, how fast the game simulates, or any shader. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimum on-screen time of one frame for this cap; 0 for "no cap" (0) or an
 * out-of-range cap, which means Metal's ordinary present. */
uint64_t skyrim_present_min_duration_ns(unsigned fps_limit);

/* 1 when the display refresh divides evenly into this cap, so every frame can
 * stay on screen for the same number of refreshes (60 on 60/120 Hz, 30 on
 * 60/120 Hz, 40 and 24 on 120 Hz). 0 for no cap (every refresh), an unknown
 * refresh, or a cap the refresh cannot hold evenly (24 or 40 on a 60 Hz screen). */
int skyrim_present_even_cadence(unsigned display_hz, unsigned fps_limit);

/* Refresh-rate request for the OS (CADisplayLink preferredFrameRateRange). The
 * preferred value is the highest refresh the screen offers that is an integer
 * multiple of the cap; the minimum is the cap itself. Returns 0 and leaves the
 * outputs untouched when no even refresh exists or there is no cap. */
int skyrim_present_refresh_request(unsigned display_hz, unsigned fps_limit,
                                   unsigned *minimum, unsigned *maximum, unsigned *preferred);

/* CPU pacer deadline when presents are spaced by Metal: a half period earlier
 * than the cap's own deadline, never before `now`. With sync off the deadline is
 * returned unchanged, so the established pacing is bit-for-bit preserved. */
uint64_t skyrim_present_cpu_deadline(uint64_t deadline, uint64_t now, uint64_t period_ns, int sync);

/* Frame cap as seen by the Metal layer's present path. The pacer stores it each
 * frame; the present hook reads it on a Metal-owned thread. Relaxed atomics. */
void skyrim_present_set_target(uint64_t min_duration_ns);
uint64_t skyrim_present_target(void);

/* 1 when SKYRIM_IOS_DISPLAY_SYNC is exactly "1" (read once, never changes). */
int skyrim_present_sync_enabled(void);

#ifdef __cplusplus
}
#endif
