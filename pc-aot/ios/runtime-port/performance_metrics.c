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
    "resource_wait", "d3d_shader_wait",
    "image_acquire", "queue_present", "queue_submit", "fence_wait", "submission_wait",
    "mvk_msl_wait", "mvk_specialize_wait", "mvk_pipeline_wait", "mvk_other_wait",
    "mvk_dispatch_wait", "mvk_dispatch_block",
    "write_file", "write_file_ex", "save_candidate_write", "save_candidate_write_ex",
    "save_candidate_window"
};
typedef struct PerfAtomic {
    _Atomic uint64_t buckets[SKYRIM_PERF_BUCKET_COUNT];
    _Atomic uint64_t total_ns, max_ns, failures, sleep_calls;
} PerfAtomic;
static PerfAtomic counters[SKYRIM_PERF_KIND_COUNT];

static atomic_flag stall_lock = ATOMIC_FLAG_INIT;
static SkyrimPerfStall stall_frames[SKYRIM_PERF_STALL_FRAMES];
static SkyrimPerfStall stall_work[SKYRIM_PERF_STALL_WORK];
static unsigned stall_frame_count, stall_work_count;
static _Atomic uint64_t stall_dropped_busy, stall_dropped_full;
/* 0 admits initial foreground startup; UINT64_MAX is inactive/clock failure. */
static _Atomic uint64_t active_since_ns;
static _Atomic uint64_t next_thread = 1;
static _Thread_local uint64_t thread_ordinal;

static uint64_t anonymous_thread(void) {
    if (!thread_ordinal) {
        uint64_t next = atomic_load_explicit(&next_thread, memory_order_relaxed);
        /* One attempt: contention may leave this sample's ordinal unavailable.
         * Saturation never wraps into an old identity. Try again next time. */
        if (next != UINT64_MAX && atomic_compare_exchange_strong_explicit(
                &next_thread, &next, next + 1, memory_order_relaxed, memory_order_relaxed))
            thread_ordinal = next;
    }
    return thread_ordinal;
}

static void stall_record(unsigned kind, uint64_t started, uint64_t ended,
                         int failed, unsigned estimated_end) {
    if (!started || ended < started || ended - started < SKYRIM_PERF_STALL_THRESHOLD_NS)
        return;
    if (started < atomic_load_explicit(&active_since_ns, memory_order_acquire)) return;
    if (atomic_flag_test_and_set_explicit(&stall_lock, memory_order_acquire)) {
        atomic_fetch_add_explicit(&stall_dropped_busy, 1, memory_order_relaxed);
        return;
    }
    if (started >= atomic_load_explicit(&active_since_ns, memory_order_acquire)) {
        unsigned *count = kind == SKYRIM_PERF_FRAME_GAP ? &stall_frame_count : &stall_work_count;
        unsigned capacity = kind == SKYRIM_PERF_FRAME_GAP ? SKYRIM_PERF_STALL_FRAMES : SKYRIM_PERF_STALL_WORK;
        SkyrimPerfStall *spans = kind == SKYRIM_PERF_FRAME_GAP ? stall_frames : stall_work;
        if (*count < capacity) {
            /* A file lifetime may cross threads; do not attribute its elapsed
             * window to the thread that happened to close the handle. */
            spans[(*count)++] = (SkyrimPerfStall){started, ended,
                kind == SKYRIM_PERF_SAVE_CANDIDATE_WINDOW ? 0 : anonymous_thread(),
                                                kind, !!failed, estimated_end};
        } else {
            atomic_fetch_add_explicit(&stall_dropped_full, 1, memory_order_relaxed);
        }
    }
    atomic_flag_clear_explicit(&stall_lock, memory_order_release);
}

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

static void histogram_record(unsigned kind, uint64_t elapsed, int failed, uint64_t sleeps) {
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

void skyrim_perf_record(unsigned kind, uint64_t elapsed, int failed, uint64_t sleeps) {
    if (kind >= SKYRIM_PERF_KIND_COUNT) return;
    histogram_record(kind, elapsed, failed, sleeps);
    if (elapsed >= SKYRIM_PERF_STALL_THRESHOLD_NS) {
        uint64_t now = skyrim_perf_now_ns();
        if (now > elapsed) stall_record(kind, now - elapsed, now, failed, 1);
    }
}

void skyrim_perf_interval(unsigned kind, uint64_t started, uint64_t ended, int failed, uint64_t sleeps) {
    if (kind >= SKYRIM_PERF_KIND_COUNT || !started || ended < started) return;
    histogram_record(kind, ended - started, failed, sleeps);
    stall_record(kind, started, ended, failed, 0);
}

void skyrim_perf_end(unsigned kind, uint64_t started, int failed) {
    uint64_t now = skyrim_perf_now_ns();
    skyrim_perf_interval(kind, started, now, failed, 0);
}

void skyrim_perf_activity(int active) {
    uint64_t now = active ? skyrim_perf_now_ns() : 0;
    atomic_store_explicit(&active_since_ns, now ? now : UINT64_MAX, memory_order_release);
}

int skyrim_perf_stalls_take(SkyrimPerfStalls *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (atomic_flag_test_and_set_explicit(&stall_lock, memory_order_acquire)) return 0;
    out->count = stall_frame_count + stall_work_count;
    memcpy(out->spans, stall_frames, stall_frame_count * sizeof(stall_frames[0]));
    memcpy(out->spans + stall_frame_count, stall_work, stall_work_count * sizeof(stall_work[0]));
    stall_frame_count = stall_work_count = 0;
    out->dropped_busy = atomic_exchange_explicit(&stall_dropped_busy, 0, memory_order_relaxed);
    out->dropped_full = atomic_exchange_explicit(&stall_dropped_full, 0, memory_order_relaxed);
    atomic_flag_clear_explicit(&stall_lock, memory_order_release);
    return 1;
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
                (kind >= SKYRIM_PERF_READ_FILE && kind <= SKYRIM_PERF_XWMA_DECODE) ||
                (kind >= SKYRIM_PERF_IMAGE_ACQUIRE && kind <= SKYRIM_PERF_SUBMISSION_WAIT) ||
                (kind >= SKYRIM_PERF_WRITE_FILE && kind <= SKYRIM_PERF_SAVE_CANDIDATE_WRITE_EX),
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
    SkyrimPerfStalls stalls;
    if (skyrim_perf_stalls_take(&stalls) &&
            (stalls.count || stalls.dropped_busy || stalls.dropped_full)) {
        /* Formatting and file I/O are outside the recording lock, at the same
         * five-second reporting cadence. Numeric spans contain no game data. */
        char line[3072];
        int used = snprintf(line, sizeof(line),
            "IOS_STALL schema=1 threshold_ns=%llu count=%u dropped_busy=%llu dropped_full=%llu fields=kind,thread,start_ns,end_ns,failed,estimated_end spans=",
            (unsigned long long)SKYRIM_PERF_STALL_THRESHOLD_NS, stalls.count,
            (unsigned long long)stalls.dropped_busy, (unsigned long long)stalls.dropped_full);
        if (used < 0 || (size_t)used >= sizeof(line)) return;
        for (unsigned i = 0; i < stalls.count; ++i) {
            const SkyrimPerfStall *s = &stalls.spans[i];
            int n = snprintf(line + used, sizeof(line) - (size_t)used, "%s%u,%llu,%llu,%llu,%u,%u",
                i ? ";" : "", s->kind, (unsigned long long)s->thread,
                (unsigned long long)s->started_ns, (unsigned long long)s->ended_ns,
                s->failed, s->estimated_end);
            if (n < 0 || (size_t)n >= sizeof(line) - (size_t)used) { used = -1; break; }
            used += n;
        }
        if (used >= 0 && (size_t)used + 2 <= sizeof(line)) {
            line[used++] = '\n'; line[used] = 0; fputs(line, stderr);
        }
    }
}
