/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Observation-only hooks on Metal's own public callbacks. The pinned MoltenVK
 * sources are not modified (their native submission baseline is byte-pinned and
 * part of the shader-cache identity); the host instead observes through:
 *   - CAMetalLayer -nextDrawable: registers -addPresentedHandler: to read the
 *     real presented time of each drawable,
 *   - the concrete MTLCommandQueue class: registers -addCompletedHandler: on each
 *     command buffer to read GPUStartTime/GPUEndTime.
 * Neither changes a wait, a result, a submission order or an error path.
 * Set SKYRIM_IOS_DISPLAY_TIMING=0 to install nothing. */
typedef struct SkyrimDisplayTimingStatus {
    unsigned disabled;           /* 1: switched off by the environment, nothing installed */
    unsigned presented_installed;/* 1: nextDrawable observed */
    unsigned gpu_methods;        /* number of command-buffer factory methods observed (0..3) */
} SkyrimDisplayTimingStatus;

/* Call once, on the main thread, before the guest starts. Prints one
 * IOS_DISPLAY_TIMING line stating exactly what was installed; an observer that
 * could not be installed is reported as absent, never as success. */
SkyrimDisplayTimingStatus skyrim_display_timing_install(void);

/* Opt-in pacing (SKYRIM_IOS_DISPLAY_SYNC=1 only). Tells the OS which refresh rate suits this
 * frame cap (CADisplayLink preferredFrameRateRange). Any thread; cheap when the cap is
 * unchanged; applied on the main thread. No effect unless the option is on. */
void skyrim_present_refresh_hint(unsigned fps_limit);

#ifdef __cplusplus
}
#endif
