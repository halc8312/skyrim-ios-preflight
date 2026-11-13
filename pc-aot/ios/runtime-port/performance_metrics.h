/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SkyrimPerfKind {
    SKYRIM_PERF_FRAME_GAP,
    SKYRIM_PERF_PRESENT_HOST,
    SKYRIM_PERF_PACER,
    SKYRIM_PERF_HLSL,
    SKYRIM_PERF_GRAPHICS_PIPELINE,
    SKYRIM_PERF_COMPUTE_PIPELINE,
    SKYRIM_PERF_SHADER_MODULE,
    SKYRIM_PERF_KIND_COUNT
};
#define SKYRIM_PERF_BUCKET_COUNT 20
typedef struct SkyrimPerfWindow {
    uint64_t buckets[SKYRIM_PERF_BUCKET_COUNT];
    uint64_t total_ns, max_ns, failures, sleep_calls;
} SkyrimPerfWindow;

/* Host wall time only, not GPU execution or CPU utilization. No allocations,
 * file I/O or guest-memory access occur in the recording functions. */
uint64_t skyrim_perf_now_ns(void);
void skyrim_perf_record(unsigned kind, uint64_t elapsed_ns, int failed, uint64_t sleep_calls);
void skyrim_perf_end(unsigned kind, uint64_t started_ns, int failed);
uint64_t skyrim_perf_bucket_upper_ns(unsigned bucket);
/* One reader. Atomic fields may straddle an interval during concurrent writes;
 * windows are approximate, with no counters discarded between windows. */
void skyrim_perf_take(unsigned kind, SkyrimPerfWindow *out);
void skyrim_perf_log_window(void);

#ifdef __cplusplus
}
#endif
