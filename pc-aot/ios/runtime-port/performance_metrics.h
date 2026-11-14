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
    SKYRIM_PERF_GRAPHICS_COMPILE_WAIT,
    SKYRIM_PERF_COMPUTE_COMPILE_WAIT,
    SKYRIM_PERF_READ_FILE,
    SKYRIM_PERF_READ_FILE_EX,
    SKYRIM_PERF_D3D_SHADER_CREATE,
    SKYRIM_PERF_TEXTURE_LOAD,
    SKYRIM_PERF_XWMA_DECODE,
    SKYRIM_PERF_RESOURCE_WAIT,
    SKYRIM_PERF_D3D_SHADER_WAIT,
    SKYRIM_PERF_IMAGE_ACQUIRE,
    SKYRIM_PERF_QUEUE_PRESENT,
    SKYRIM_PERF_QUEUE_SUBMIT,
    SKYRIM_PERF_FENCE_WAIT,
    SKYRIM_PERF_SUBMISSION_WAIT,
    SKYRIM_PERF_MVK_MSL_WAIT,
    SKYRIM_PERF_MVK_SPECIALIZE_WAIT,
    SKYRIM_PERF_MVK_PIPELINE_WAIT,
    SKYRIM_PERF_MVK_OTHER_WAIT,
    SKYRIM_PERF_MVK_DISPATCH_WAIT,
    SKYRIM_PERF_MVK_DISPATCH_BLOCK,
    SKYRIM_PERF_WRITE_FILE,
    SKYRIM_PERF_WRITE_FILE_EX,
    SKYRIM_PERF_SAVE_CANDIDATE_WRITE,
    SKYRIM_PERF_SAVE_CANDIDATE_WRITE_EX,
    SKYRIM_PERF_SAVE_CANDIDATE_WINDOW,
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
/* Use the original interval when available; avoids a second clock read and
 * lets long operations be correlated with frame gaps. Invalid intervals drop. */
void skyrim_perf_interval(unsigned kind, uint64_t started_ns, uint64_t ended_ns,
                          int failed, uint64_t sleep_calls);
void skyrim_perf_end(unsigned kind, uint64_t started_ns, int failed);
uint64_t skyrim_perf_bucket_upper_ns(unsigned bucket);
/* One reader. Atomic fields may straddle an interval during concurrent writes;
 * windows are approximate, with no counters discarded between windows. */
void skyrim_perf_take(unsigned kind, SkyrimPerfWindow *out);
void skyrim_perf_log_window(void);

#define SKYRIM_PERF_STALL_THRESHOLD_NS UINT64_C(50000000)
#define SKYRIM_PERF_STALL_FRAMES 4
#define SKYRIM_PERF_STALL_WORK 12
#define SKYRIM_PERF_STALL_CAPACITY (SKYRIM_PERF_STALL_FRAMES + SKYRIM_PERF_STALL_WORK)
typedef struct SkyrimPerfStall {
    uint64_t started_ns, ended_ns, thread;
    unsigned kind, failed, estimated_end;
} SkyrimPerfStall;
typedef struct SkyrimPerfStalls {
    SkyrimPerfStall spans[SKYRIM_PERF_STALL_CAPACITY];
    unsigned count;
    uint64_t dropped_busy, dropped_full;
} SkyrimPerfStalls;
/* Fixed samples, not a complete trace. Writers try once and never wait; four
 * frame slots are reserved so nested compilation cannot crowd out every gap.
 * Anonymous thread ordinals are process-local (0 means unavailable), never
 * pthread IDs or addresses. Recording allocates nothing and writes no files.
 * Only elapsed-only record() uses an estimated endpoint. */
int skyrim_perf_stalls_take(SkyrimPerfStalls *out);
/* Called at UIKit activity changes. Ignore spans crossing an inactive interval;
 * this affects stall samples only, not the established histogram contract. */
void skyrim_perf_activity(int active);

#ifdef __cplusplus
}
#endif
