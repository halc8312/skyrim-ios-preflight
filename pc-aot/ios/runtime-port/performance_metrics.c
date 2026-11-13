/* SPDX-License-Identifier: GPL-3.0-only */
#include "performance_metrics.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const uint64_t upper_ns[SKYRIM_PERF_BUCKET_COUNT] = {
    0, 1000000, 2000000, 4000000, 8000000, 12000000, 16666667,
    20000000, 25000000, 33333334, 40000000, 50000000, 66666667,
    100000000, 166666667, 250000000, 500000000, 1000000000,
    2000000000, UINT64_MAX
};
static const char *const names[SKYRIM_PERF_KIND_COUNT] = {
    "frame_gap", "present_host", "pacer", "hlsl",
    "graphics_pipeline", "compute_pipeline", "shader_module",
    "graphics_compile_wait", "compute_compile_wait",
    "read_file", "read_file_ex", "d3d_shader_create", "texture_load", "xwma_decode",
    "resource_wait", "d3d_shader_wait"
};
typedef struct PerfAtomic {
    _Atomic uint64_t buckets[SKYRIM_PERF_BUCKET_COUNT];
    _Atomic uint64_t total_ns, max_ns, failures, sleep_calls;
} PerfAtomic;
static PerfAtomic counters[SKYRIM_PERF_KIND_COUNT];

uint64_t skyrim_perf_now_ns(void) {
    struct timespec now;
    const int saved_errno = errno;
    const int result = clock_gettime(CLOCK_MONOTONIC, &now);
    errno = saved_errno;
    if (result) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

uint64_t skyrim_perf_bucket_upper_ns(unsigned bucket) {
    return upper_ns[bucket < SKYRIM_PERF_BUCKET_COUNT ? bucket : SKYRIM_PERF_BUCKET_COUNT - 1];
}

void skyrim_perf_record(unsigned kind, uint64_t elapsed, int failed, uint64_t sleeps) {
    if (kind >= SKYRIM_PERF_KIND_COUNT) return;
    unsigned lo = 0, hi = SKYRIM_PERF_BUCKET_COUNT - 1;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (elapsed <= upper_ns[mid]) hi = mid;
        else lo = mid + 1;
    }
    PerfAtomic *p = &counters[kind];
    atomic_fetch_add_explicit(&p->buckets[lo], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&p->total_ns, elapsed, memory_order_relaxed);
    if (failed) atomic_fetch_add_explicit(&p->failures, 1, memory_order_relaxed);
    if (sleeps) atomic_fetch_add_explicit(&p->sleep_calls, sleeps, memory_order_relaxed);
    uint64_t old = atomic_load_explicit(&p->max_ns, memory_order_relaxed);
    while (old < elapsed && !atomic_compare_exchange_weak_explicit(
        &p->max_ns, &old, elapsed, memory_order_relaxed, memory_order_relaxed)) {}
}

void skyrim_perf_end(unsigned kind, uint64_t started, int failed) {
    uint64_t now = skyrim_perf_now_ns();
    if (started && now >= started) skyrim_perf_record(kind, now - started, failed, 0);
}

void skyrim_perf_take(unsigned kind, SkyrimPerfWindow *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (kind >= SKYRIM_PERF_KIND_COUNT) return;
    PerfAtomic *p = &counters[kind];
    for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i)
        out->buckets[i] = atomic_exchange_explicit(&p->buckets[i], 0, memory_order_relaxed);
    out->total_ns = atomic_exchange_explicit(&p->total_ns, 0, memory_order_relaxed);
    out->max_ns = atomic_exchange_explicit(&p->max_ns, 0, memory_order_relaxed);
    out->failures = atomic_exchange_explicit(&p->failures, 0, memory_order_relaxed);
    out->sleep_calls = atomic_exchange_explicit(&p->sleep_calls, 0, memory_order_relaxed);
}

void skyrim_perf_log_window(void) {
    static uint64_t previous_ns; // UI reporter is the only reader
    uint64_t end_ns = skyrim_perf_now_ns();
    uint64_t window_ns = previous_ns && end_ns >= previous_ns ? end_ns - previous_ns : 0;
    previous_ns = end_ns;
    for (unsigned kind = 0; kind < SKYRIM_PERF_KIND_COUNT; ++kind) {
        SkyrimPerfWindow w;
        skyrim_perf_take(kind, &w);
        uint64_t count = 0;
        for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i) count += w.buckets[i];
        if (!count && !w.total_ns && !w.max_ns && !w.failures && !w.sleep_calls) continue;
        /* One bounded write per active category, once per five-second report.
         * The fixed histogram schema is documented; no shader names or bytes. */
        char line[1024];
        int used = snprintf(line, sizeof(line),
            "IOS_TIMING schema=1 approximate_window=1 end_ns=%llu window_ns=%llu kind=%s samples=%llu total_ns=%llu max_ns=%llu failures_known=%u failures=%llu sleep_calls=%llu bins=",
            (unsigned long long)end_ns, (unsigned long long)window_ns,
            names[kind], (unsigned long long)count, (unsigned long long)w.total_ns,
            (unsigned long long)w.max_ns,
            (kind >= SKYRIM_PERF_HLSL && kind <= SKYRIM_PERF_SHADER_MODULE) ||
                (kind >= SKYRIM_PERF_READ_FILE && kind <= SKYRIM_PERF_XWMA_DECODE),
            (unsigned long long)w.failures, (unsigned long long)w.sleep_calls);
        if (used < 0 || (size_t)used >= sizeof(line)) continue;
        for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i) {
            int n = snprintf(line + used, sizeof(line) - (size_t)used, "%s%llu",
                i ? "," : "", (unsigned long long)w.buckets[i]);
            if (n < 0 || (size_t)n >= sizeof(line) - (size_t)used) { used = -1; break; }
            used += n;
        }
        if (used >= 0 && (size_t)used + 2 <= sizeof(line)) {
            line[used++] = '\n'; line[used] = 0; fputs(line, stderr);
        }
    }
}
