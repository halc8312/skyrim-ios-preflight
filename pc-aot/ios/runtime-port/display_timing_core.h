/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Display and GPU timing core: integer arithmetic only, no Apple headers.
 *
 * Metal reports presented and GPU times on its own clock (CACurrentMediaTime,
 * mach_absolute_time based), which differs from the host monotonic clock used
 * by every other IOS_TIMING record by the time the device spent asleep. The
 * callers pass both clocks read together, so a presented time can be mapped to
 * the host clock without assuming the two clocks share an origin.
 *
 * Every function is safe to call from Metal's internal callback threads: atomics
 * and the existing bounded recorder only; no locks, allocation or file I/O.
 * Nothing here observes game memory or controls frame timing. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One presented-handler callback. All values are nanoseconds on the Metal media
 * clock except now_host_ns (the host monotonic clock, read next to now_media_ns).
 * presented_media_ns == 0 means Metal reports the frame was not shown. */
void skyrim_display_presented(uint64_t presented_media_ns, uint64_t acquired_media_ns,
                              uint64_t now_media_ns, uint64_t now_host_ns);

/* One completed command buffer. ok is 0 when Metal reports an error, or when
 * GPUStartTime/GPUEndTime are unusable (a buffer that never ran reports 0). */
void skyrim_display_gpu_completed(uint64_t gpu_start_media_ns, uint64_t gpu_end_media_ns, int ok);

/* Map a media-clock instant to the host clock: now_host - (now_media - t).
 * Returns 0 when it cannot be mapped (an instant in the future, or before the
 * host clock origin). Exposed for tests. */
uint64_t skyrim_display_to_host_ns(uint64_t media_ns, uint64_t now_media_ns, uint64_t now_host_ns);

/* Forget the previous presented time and any partial GPU totals, e.g. after
 * the app was inactive: an interval across suspension is not a display
 * interval. Safe to call from any thread. */
void skyrim_display_reset(void);

#ifdef __cplusplus
}
#endif
