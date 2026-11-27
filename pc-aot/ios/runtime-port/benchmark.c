/* SPDX-License-Identifier: GPL-3.0-only */
#include "benchmark.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define NS_PER_MS UINT64_C(1000000)
#define NS_PER_SECOND UINT64_C(1000000000)
#define HISTOGRAM_LIMIT_MS 2000u

typedef struct {
    unsigned milliseconds;
    SkyrimMobilePad pad;
} Step;

static const Step turns[] = {
    {0, {0}}, {2000, {.rx = 15500}}, {9000, {.rx = -15500}}, {16000, {0}},
};
static const Step movement[] = {
    {0, {0}}, {2000, {.ly = 26000}}, {17000, {0}},
};
static const Step magic[] = {
    {0, {0}}, {2000, {.lt = 255}}, {3300, {0}}, {7000, {.lt = 255, .rt = 255}},
    {8300, {0}}, {12000, {.lt = 255, .rt = 255}}, {13300, {0}},
};
static const Step still[] = {{0, {0}}};

static const struct {
    const Step *steps;
    unsigned count;
} scenes[] = {
    {NULL, 0}, {turns, 4}, {movement, 3}, {still, 1},
    {magic, 7}, {still, 1}, {magic, 7},
};
_Static_assert(sizeof scenes / sizeof scenes[0] == SKYRIM_BENCH_LAST + 1, "scene table");
_Static_assert(sizeof magic / sizeof magic[0] <= SKYRIM_BENCH_STEPS, "phase storage");

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SkyrimBenchmarkResult result;
static uint64_t start_ns, end_ns, last_frame_ns;
static unsigned histogram[HISTOGRAM_LIMIT_MS + 1];
static unsigned run_number;
static bool active, pending, resized;
static uint64_t render_size;

int skyrim_benchmark_active(void) {
    return __atomic_load_n(&active, __ATOMIC_ACQUIRE);
}

void skyrim_benchmark_get_render_size(unsigned *width, unsigned *height) {
    uint64_t size = __atomic_load_n(&render_size, __ATOMIC_ACQUIRE);
    *width = (unsigned)(size >> 32);
    *height = (unsigned)size;
}

int skyrim_benchmark_start(unsigned scene, uint64_t now, unsigned selected_height,
                           unsigned cap, unsigned thermal, unsigned low_power) {
    if (scene < SKYRIM_BENCH_TURN || scene > SKYRIM_BENCH_LAST ||
        now > UINT64_MAX - (SKYRIM_BENCH_COUNTDOWN_SECONDS + SKYRIM_BENCH_DURATION_SECONDS + 15) * NS_PER_SECOND)
        return 0;
    pthread_mutex_lock(&lock);
    if (skyrim_benchmark_active() || pending) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    unsigned width, height;
    skyrim_benchmark_get_render_size(&width, &height);
    start_ns = now + SKYRIM_BENCH_COUNTDOWN_SECONDS * NS_PER_SECOND;
    end_ns = start_ns + SKYRIM_BENCH_DURATION_SECONDS * NS_PER_SECOND;
    result = (SkyrimBenchmarkResult){
        .run = ++run_number, .scene = scene, .started_ns = start_ns,
        .width = width, .height = height,
        .selected_height = selected_height, .cap_start = cap, .cap_end = cap,
        .thermal_start = thermal, .thermal_max = thermal, .low_power = (low_power != 0),
    };
    last_frame_ns = 0;
    resized = false;
    memset(histogram, 0, sizeof histogram);
    __atomic_store_n(&active, true, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
    return 1;
}

static void finish(unsigned status, uint64_t now) {
    result.status = status;
    result.finished_ns = now;
    if (result.intervals) {
        unsigned rank = result.intervals - result.intervals / 100;
        unsigned cumulative = 0;
        for (unsigned i = 0; i <= HISTOGRAM_LIMIT_MS; ++i) {
            cumulative += histogram[i];
            if (cumulative >= rank) {
                result.p99_ms = i;
                break;
            }
        }
    }
    pending = true;
    __atomic_store_n(&active, false, __ATOMIC_RELEASE);
}

void skyrim_benchmark_cancel(uint64_t now) {
    if (!skyrim_benchmark_active()) return;
    pthread_mutex_lock(&lock);
    if (skyrim_benchmark_active()) finish(SKYRIM_BENCH_INTERRUPTED, now);
    pthread_mutex_unlock(&lock);
}

void skyrim_benchmark_render_size(unsigned width, unsigned height) {
    uint64_t size = ((uint64_t)width << 32) | height;
    pthread_mutex_lock(&lock);
    uint64_t old = __atomic_exchange_n(&render_size, size, __ATOMIC_ACQ_REL);
    if (skyrim_benchmark_active() && size != old) resized = true;
    pthread_mutex_unlock(&lock);
    if (size != old && width && height)
        fprintf(stderr, "IOS_RENDER_BUFFER width=%u height=%u\n", width, height);
}

int skyrim_benchmark_pad(uint64_t now, SkyrimMobilePad *pad) {
    if (!skyrim_benchmark_active()) return 0;
    pthread_mutex_lock(&lock);
    int owns_pad = skyrim_benchmark_active() && now >= start_ns;
    if (owns_pad) {
        *pad = (SkyrimMobilePad){0};
        if (now < end_ns) {
            const Step *steps = scenes[result.scene].steps;
            unsigned elapsed = (unsigned)((now - start_ns) / NS_PER_MS);
            unsigned i = scenes[result.scene].count - 1;
            while (elapsed < steps[i].milliseconds) --i;
            *pad = steps[i].pad;
            if (!(result.seen & (1u << i))) {
                result.seen |= 1u << i;
                result.phase_ms[i] = elapsed;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return owns_pad;
}

void skyrim_benchmark_frame(uint64_t now) {
    if (!skyrim_benchmark_active()) return;
    pthread_mutex_lock(&lock);
    if (skyrim_benchmark_active() && now >= start_ns && now > last_frame_ns) {
        if (last_frame_ns) {
            /* Include a stall that started inside the window and ended past
             * the deadline. Exclude the gap preceding the first measured frame. */
            uint64_t gap = now - last_frame_ns;
            uint64_t ms = gap / NS_PER_MS + (gap % NS_PER_MS != 0);
            ++histogram[ms < HISTOGRAM_LIMIT_MS ? (unsigned)ms : HISTOGRAM_LIMIT_MS];
            ++result.intervals;
            result.total_ns += gap;
            if (gap > result.max_ns) result.max_ns = gap;
            result.over100 += gap > 100 * NS_PER_MS;
        }
        last_frame_ns = now;
        if (now >= end_ns) {
            unsigned expected = (1u << scenes[result.scene].count) - 1;
            unsigned status = result.intervals && result.seen == expected
                ? SKYRIM_BENCH_COMPLETE : SKYRIM_BENCH_INCOMPLETE;
            finish(resized ? SKYRIM_BENCH_RESIZED : status, now);
        }
    }
    pthread_mutex_unlock(&lock);
}

unsigned skyrim_benchmark_tick(uint64_t now, unsigned cap, unsigned thermal) {
    if (!skyrim_benchmark_active()) return 0;
    pthread_mutex_lock(&lock);
    unsigned countdown = 0;
    if (skyrim_benchmark_active()) {
        result.cap_end = cap;
        if (thermal > result.thermal_max) result.thermal_max = thermal;
        if (now >= end_ns + 15 * NS_PER_SECOND) finish(SKYRIM_BENCH_INCOMPLETE, now);
        else if (now < start_ns)
            countdown = (unsigned)((start_ns - now + NS_PER_SECOND - 1) / NS_PER_SECOND);
    }
    pthread_mutex_unlock(&lock);
    return countdown;
}

int skyrim_benchmark_take_result(SkyrimBenchmarkResult *out) {
    pthread_mutex_lock(&lock);
    int ready = pending;
    if (ready) {
        *out = result;
        pending = false;
    }
    pthread_mutex_unlock(&lock);
    return ready;
}
