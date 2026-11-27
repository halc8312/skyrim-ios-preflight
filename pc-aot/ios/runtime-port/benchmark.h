/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "mobile_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SKYRIM_BENCH_TURN = 1,
    SKYRIM_BENCH_MOVE,
    SKYRIM_BENCH_STILL,
    SKYRIM_BENCH_MAGIC,
    SKYRIM_BENCH_NPCS,
    SKYRIM_BENCH_NPCS_MAGIC,
    SKYRIM_BENCH_LAST = SKYRIM_BENCH_NPCS_MAGIC
} SkyrimBenchmarkScene;

typedef enum {
    SKYRIM_BENCH_COMPLETE = 1,
    SKYRIM_BENCH_INTERRUPTED,
    SKYRIM_BENCH_INCOMPLETE,
    SKYRIM_BENCH_RESIZED
} SkyrimBenchmarkStatus;

#define SKYRIM_BENCH_COUNTDOWN_SECONDS 5
#define SKYRIM_BENCH_DURATION_SECONDS 20
#define SKYRIM_BENCH_STEPS 8

typedef struct {
    unsigned run, scene, status, intervals, over100, p99_ms, width, height;
    unsigned selected_height, cap_start, cap_end, thermal_start, thermal_max, low_power;
    uint64_t total_ns, max_ns, started_ns, finished_ns;
    unsigned seen, phase_ms[SKYRIM_BENCH_STEPS];
} SkyrimBenchmarkResult;

/* UI starts after dismissal and owns result consumption. Present and XInput
 * can run on different threads. All time arguments use the monotonic clock. */
int skyrim_benchmark_start(unsigned scene, uint64_t now, unsigned selected_height,
                           unsigned cap, unsigned thermal, unsigned low_power);
int skyrim_benchmark_active(void);
void skyrim_benchmark_cancel(uint64_t now);
void skyrim_benchmark_render_size(unsigned width, unsigned height);
void skyrim_benchmark_get_render_size(unsigned *width, unsigned *height);
int skyrim_benchmark_pad(uint64_t now, SkyrimMobilePad *pad);
void skyrim_benchmark_frame(uint64_t now);
unsigned skyrim_benchmark_tick(uint64_t now, unsigned cap, unsigned thermal);
int skyrim_benchmark_take_result(SkyrimBenchmarkResult *result);

#ifdef __cplusplus
}
#endif
