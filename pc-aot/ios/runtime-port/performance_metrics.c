/* SPDX-License-Identifier: GPL-3.0-only */
#include "performance_metrics.h"
#include <errno.h>
#include <inttypes.h>
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
    "save_candidate_window", "mvk_cold_lock_wait", "mvk_convert",
    "pacer_runloop", "pacer_sleep", "ui_pump", "pacer_overrun", "graphics_state_wait",
    "save_first_to_last_write_start", "save_last_write_start_to_close", "save_close_call",
    "display_interval", "display_dropped", "display_latency",
    "gpu_command_buffer", "gpu_frame_busy", "gpu_frame_span"
};
_Static_assert(sizeof names / sizeof names[0] == SKYRIM_PERF_KIND_COUNT,
               "every kind needs a log name; append new names at the end");
typedef struct PerfAtomic {
    _Atomic uint64_t buckets[SKYRIM_PERF_BUCKET_COUNT];
    _Atomic uint64_t total_ns, max_ns, failures, sleep_calls;
} PerfAtomic;
static PerfAtomic counters[SKYRIM_PERF_KIND_COUNT];
/* 12 MSL (role/source), 18 DXVK waits (role/result), 3 cold-lock waits,
 * 6 Vulkan pipeline calls (graphics/compute by role), 3 SPIRV-Cross conversions,
 * 9 exact graphics-state waits (role/result).
 * Fixed storage and the
 * existing relaxed atomic recorder; no extra clocks, locks or allocations. */
#define CONTEXT_COUNT 51
static PerfAtomic context_counters[CONTEXT_COUNT];
static _Thread_local SkyrimPerfContext work_context;
static _Thread_local SkyrimPerfPrefetch prefetch_context;
SkyrimPerfPrefetch skyrim_perf_prefetch_get(void) { return prefetch_context; }
SkyrimPerfPrefetch skyrim_perf_prefetch_set(SkyrimPerfPrefetch next) {
    SkyrimPerfPrefetch previous = prefetch_context;
    prefetch_context = next;
    return previous;
}
SkyrimPerfContext skyrim_perf_context_get(void) { return work_context; }
SkyrimPerfContext skyrim_perf_context_set(SkyrimPerfContext next) {
    SkyrimPerfContext old = work_context;
    if (next.role > SKYRIM_PERF_ROLE_PREFETCH) next.role = 0;
    if (next.source > SKYRIM_PERF_SOURCE_MACRO) next.source = 0;
    if (next.result > SKYRIM_PERF_WAIT_CREATE) next.result = 0;
    work_context = next;
    return old;
}
static unsigned context_index(unsigned kind, SkyrimPerfContext c) {
    if (c.role > 2 || c.source > 3 || c.result > 2) return CONTEXT_COUNT;
    if (kind == SKYRIM_PERF_MVK_MSL_WAIT) return c.role * 4 + c.source;
    if (kind == SKYRIM_PERF_GRAPHICS_COMPILE_WAIT) return 12 + c.role * 3 + c.result;
    if (kind == SKYRIM_PERF_COMPUTE_COMPILE_WAIT) return 21 + c.role * 3 + c.result;
    if (kind == SKYRIM_PERF_MVK_COLD_LOCK_WAIT) return 30 + c.role;
    if (kind == SKYRIM_PERF_GRAPHICS_PIPELINE) return 33 + c.role;
    if (kind == SKYRIM_PERF_COMPUTE_PIPELINE) return 36 + c.role;
    if (kind == SKYRIM_PERF_MVK_CONVERT) return 39 + c.role;
    if (kind == SKYRIM_PERF_GRAPHICS_STATE_WAIT) return 42 + c.role * 3 + c.result;
    return CONTEXT_COUNT;
}
static unsigned context_decode(unsigned index, SkyrimPerfContext *c) {
    *c = (SkyrimPerfContext){0};
    if (index < 12) { c->role = index / 4; c->source = index % 4; return SKYRIM_PERF_MVK_MSL_WAIT; }
    if (index < 30) {
        unsigned local = (index - 12) % 9;
        c->role = local / 3; c->result = local % 3;
        return index < 21 ? SKYRIM_PERF_GRAPHICS_COMPILE_WAIT : SKYRIM_PERF_COMPUTE_COMPILE_WAIT;
    }
    if (index < 33) {
        c->role = index - 30;
        return SKYRIM_PERF_MVK_COLD_LOCK_WAIT;
    }
    if (index >= 42) {
        c->role = (index - 42) / 3; c->result = (index - 42) % 3;
        return SKYRIM_PERF_GRAPHICS_STATE_WAIT;
    }
    c->role = (index - 33) % 3;
    if (index < 36) return SKYRIM_PERF_GRAPHICS_PIPELINE;
    return index < 39 ? SKYRIM_PERF_COMPUTE_PIPELINE : SKYRIM_PERF_MVK_CONVERT;
}

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

uint64_t skyrim_perf_thread_ordinal(void) { return anonymous_thread(); }
int skyrim_perf_interval_active(uint64_t started) {
    const uint64_t active_since = atomic_load_explicit(&active_since_ns, memory_order_acquire);
    return started && active_since != UINT64_MAX && started >= active_since;
}

/* Only completed long draw waits enter this buffer. The producer is the
 * library returned by lookup; it need not have held the lock for the wait. */
typedef struct ShaderWait {
    SkyrimPerfShaderOrigin origin;
    uint64_t wait_start_ns, wait_end_ns;
    unsigned result;
} ShaderWait;
enum { SHADER_WAIT_CAPACITY = 16 };
typedef struct ShaderWaitBatch {
    ShaderWait waits[SHADER_WAIT_CAPACITY];
    unsigned count;
    uint64_t busy, full;
} ShaderWaitBatch;
static ShaderWait shader_waits[SHADER_WAIT_CAPACITY];
static unsigned shader_wait_count;
static uint64_t shader_wait_full;
static _Atomic uint64_t shader_wait_busy;
static atomic_flag shader_wait_lock = ATOMIC_FLAG_INIT;

void skyrim_perf_shader_wait(unsigned result, uint64_t wait_start_ns,
                             uint64_t wait_end_ns, SkyrimPerfShaderOrigin origin) {
    if (work_context.role != SKYRIM_PERF_ROLE_DRAW ||
        (result != SKYRIM_PERF_WAIT_HIT && result != SKYRIM_PERF_WAIT_CREATE) ||
        !wait_start_ns || wait_end_ns < wait_start_ns ||
        wait_end_ns - wait_start_ns < SKYRIM_PERF_STALL_THRESHOLD_NS ||
        !skyrim_perf_interval_active(wait_start_ns)) return;
    if (atomic_flag_test_and_set_explicit(&shader_wait_lock, memory_order_acquire)) {
        atomic_fetch_add_explicit(&shader_wait_busy, 1, memory_order_relaxed);
        return;
    }
    const uint64_t since = atomic_load_explicit(&active_since_ns, memory_order_acquire);
    if (since != UINT64_MAX && wait_start_ns >= since) {
        if (shader_wait_count < SHADER_WAIT_CAPACITY) {
            const SkyrimPerfPrefetch job = origin.prefetch;
            if (result != SKYRIM_PERF_WAIT_HIT || !job.queued_ns || job.queued_ns < since ||
                job.ready_ns > job.queued_ns || job.queued_ns > job.worker_ns ||
                job.worker_ns > origin.compile_ns || origin.compile_ns > origin.published_ns ||
                origin.published_ns > wait_end_ns) {
                origin = (SkyrimPerfShaderOrigin){0};
            } else if (job.ready_ns < since) {
                origin.prefetch.ready_ns = 0;
            }
            shader_waits[shader_wait_count++] = (ShaderWait){origin, wait_start_ns, wait_end_ns, result};
        } else {
            ++shader_wait_full;
        }
    }
    atomic_flag_clear_explicit(&shader_wait_lock, memory_order_release);
}

static int shader_waits_take(ShaderWaitBatch *out) {
    if (atomic_flag_test_and_set_explicit(&shader_wait_lock, memory_order_acquire)) return 0;
    out->count = shader_wait_count;
    memcpy(out->waits, shader_waits, out->count * sizeof(shader_waits[0]));
    out->busy = atomic_exchange_explicit(&shader_wait_busy, 0, memory_order_relaxed);
    out->full = shader_wait_full;
    shader_wait_count = 0;
    shader_wait_full = 0;
    atomic_flag_clear_explicit(&shader_wait_lock, memory_order_release);
    return 1;
}

static void log_shader_waits(void) {
    /* One reporter per process. Equal clock ticks on separate draw threads
     * remain distinct; replaying a captured record retains its sequence. */
    static uint64_t sequence;
    ShaderWaitBatch batch;
    if (!shader_waits_take(&batch)) return;
    for (unsigned i = 0; i < batch.count && sequence != UINT64_MAX; ++i) {
        const ShaderWait *w = &batch.waits[i];
        const SkyrimPerfPrefetch *job = &w->origin.prefetch;
        fprintf(stderr, "IOS_SHADER_LEAD schema=1 seq=%" PRIu64 " result=%u ready_ns=%" PRIu64
            " queued_ns=%" PRIu64 " worker_ns=%" PRIu64 " compile_ns=%" PRIu64
            " published_ns=%" PRIu64 " wait_start_ns=%" PRIu64 " wait_end_ns=%" PRIu64 "\n",
            ++sequence, w->result, job->ready_ns, job->queued_ns, job->worker_ns,
            w->origin.compile_ns, w->origin.published_ns, w->wait_start_ns, w->wait_end_ns);
    }
    if ((batch.busy || batch.full) && sequence != UINT64_MAX)
        fprintf(stderr, "IOS_SHADER_LEAD_DROPS schema=1 seq=%" PRIu64 " busy=%" PRIu64 " full=%" PRIu64 "\n",
                ++sequence, batch.busy, batch.full);
}

static void stall_record(unsigned kind, uint64_t started, uint64_t ended,
                         int failed, unsigned estimated_end, SkyrimPerfContext context) {
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
            /* The writes and close can run on different threads. Only the
             * close call itself is a measurement on the closing thread. */
            const int handle_interval = kind == SKYRIM_PERF_SAVE_CANDIDATE_WINDOW ||
                kind == SKYRIM_PERF_SAVE_FIRST_TO_LAST_WRITE_START ||
                kind == SKYRIM_PERF_SAVE_LAST_WRITE_START_TO_CLOSE;
            spans[(*count)++] = (SkyrimPerfStall){started, ended,
                handle_interval ? 0 : anonymous_thread(), kind, !!failed, estimated_end,
                handle_interval ? (SkyrimPerfContext){0} : context};
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

static void histogram_record(PerfAtomic *p, uint64_t elapsed, int failed, uint64_t sleeps) {
    unsigned lo = 0, hi = SKYRIM_PERF_BUCKET_COUNT - 1;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (elapsed <= upper_ns[mid]) hi = mid;
        else lo = mid + 1;
    }
    atomic_fetch_add_explicit(&p->buckets[lo], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&p->total_ns, elapsed, memory_order_relaxed);
    if (failed) atomic_fetch_add_explicit(&p->failures, 1, memory_order_relaxed);
    if (sleeps) atomic_fetch_add_explicit(&p->sleep_calls, sleeps, memory_order_relaxed);
    uint64_t old = atomic_load_explicit(&p->max_ns, memory_order_relaxed);
    while (old < elapsed && !atomic_compare_exchange_weak_explicit(
        &p->max_ns, &old, elapsed, memory_order_relaxed, memory_order_relaxed)) {}
}

static void record_histograms(unsigned kind, uint64_t elapsed, int failed,
                              uint64_t sleeps, SkyrimPerfContext context) {
    histogram_record(&counters[kind], elapsed, failed, sleeps);
    unsigned index = context_index(kind, context);
    if (index < CONTEXT_COUNT) histogram_record(&context_counters[index], elapsed, failed, sleeps);
}
void skyrim_perf_record(unsigned kind, uint64_t elapsed, int failed, uint64_t sleeps) {
    if (kind >= SKYRIM_PERF_KIND_COUNT) return;
    record_histograms(kind, elapsed, failed, sleeps, work_context);
    if (elapsed >= SKYRIM_PERF_STALL_THRESHOLD_NS) {
        uint64_t now = skyrim_perf_now_ns();
        if (now > elapsed) stall_record(kind, now - elapsed, now, failed, 1, work_context);
    }
}
void skyrim_perf_interval_context(unsigned kind, uint64_t started, uint64_t ended,
                                 int failed, uint64_t sleeps, SkyrimPerfContext context) {
    if (kind >= SKYRIM_PERF_KIND_COUNT || !started || ended < started) return;
    /* Nested run-loop waits can span UIKit suspension/resume even when the
     * outer pacer rejects its own sample. These new partitions describe active
     * pacing only. Preserve the established histogram contract for old kinds. */
    if (kind >= SKYRIM_PERF_PACER_RUNLOOP && kind <= SKYRIM_PERF_PACER_OVERRUN &&
            started < atomic_load_explicit(&active_since_ns, memory_order_acquire)) return;
    record_histograms(kind, ended - started, failed, sleeps, context);
    stall_record(kind, started, ended, failed, 0, context);
}
void skyrim_perf_interval(unsigned kind, uint64_t started, uint64_t ended, int failed, uint64_t sleeps) {
    skyrim_perf_interval_context(kind, started, ended, failed, sleeps, work_context);
}

static int observer_kind(unsigned kind) {
    return kind >= SKYRIM_PERF_DISPLAY_INTERVAL && kind < SKYRIM_PERF_KIND_COUNT;
}
void skyrim_perf_observe(unsigned kind, uint64_t elapsed, int failed) {
    if (!observer_kind(kind)) return;
    histogram_record(&counters[kind], elapsed, failed, 0);
}
void skyrim_perf_observe_span(unsigned kind, uint64_t started, uint64_t ended) {
    if (!observer_kind(kind) || !started || ended < started) return;
    histogram_record(&counters[kind], ended - started, 0, 0);
    if (ended - started >= SKYRIM_PERF_OBSERVE_SPAN_NS)
        stall_record(kind, started, ended, 0, 0, (SkyrimPerfContext){0});
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

static void histogram_take(PerfAtomic *p, SkyrimPerfWindow *out) {
    for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i)
        out->buckets[i] = atomic_exchange_explicit(&p->buckets[i], 0, memory_order_relaxed);
    out->total_ns = atomic_exchange_explicit(&p->total_ns, 0, memory_order_relaxed);
    out->max_ns = atomic_exchange_explicit(&p->max_ns, 0, memory_order_relaxed);
    out->failures = atomic_exchange_explicit(&p->failures, 0, memory_order_relaxed);
    out->sleep_calls = atomic_exchange_explicit(&p->sleep_calls, 0, memory_order_relaxed);
}

void skyrim_perf_take(unsigned kind, SkyrimPerfWindow *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (kind < SKYRIM_PERF_KIND_COUNT) histogram_take(&counters[kind], out);
}
void skyrim_perf_context_take(unsigned kind, SkyrimPerfContext context, SkyrimPerfWindow *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    unsigned index = context_index(kind, context);
    if (index < CONTEXT_COUNT) histogram_take(&context_counters[index], out);
}

void skyrim_perf_log_window(void) {
    static uint64_t previous_ns; // UI reporter is the only reader
    static uint64_t previous_stall_end_ns;
    uint64_t end_ns = skyrim_perf_now_ns();
    uint64_t window_ns = previous_ns && end_ns >= previous_ns ? end_ns - previous_ns : 0;
    previous_ns = end_ns;
    for (unsigned slot = 0; slot < SKYRIM_PERF_KIND_COUNT + CONTEXT_COUNT; ++slot) {
        SkyrimPerfContext context = {0};
        int attributed = slot >= SKYRIM_PERF_KIND_COUNT;
        unsigned kind = attributed ? context_decode(slot - SKYRIM_PERF_KIND_COUNT, &context) : slot;
        SkyrimPerfWindow w;
        if (attributed) skyrim_perf_context_take(kind, context, &w);
        else skyrim_perf_take(kind, &w);
        uint64_t count = 0;
        for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i) count += w.buckets[i];
        if (!count && !w.total_ns && !w.max_ns && !w.failures && !w.sleep_calls) continue;
        /* One bounded write per active category, once per five-second report.
         * The fixed histogram schema is documented; no shader names or bytes. */
        char line[1024];
        int used = snprintf(line, sizeof(line),
            "%s schema=1 approximate_window=1 end_ns=%llu window_ns=%llu kind=%s samples=%llu total_ns=%llu max_ns=%llu failures_known=%u failures=%llu sleep_calls=%llu bins=",
            attributed ? "IOS_TIMING_CONTEXT" : "IOS_TIMING",
            (unsigned long long)end_ns, (unsigned long long)window_ns,
            names[kind], (unsigned long long)count, (unsigned long long)w.total_ns,
            (unsigned long long)w.max_ns,
            (kind >= SKYRIM_PERF_HLSL && kind <= SKYRIM_PERF_SHADER_MODULE) ||
                (kind >= SKYRIM_PERF_READ_FILE && kind <= SKYRIM_PERF_XWMA_DECODE) ||
                (kind >= SKYRIM_PERF_IMAGE_ACQUIRE && kind <= SKYRIM_PERF_SUBMISSION_WAIT) ||
                (kind >= SKYRIM_PERF_WRITE_FILE && kind <= SKYRIM_PERF_SAVE_CANDIDATE_WRITE_EX) ||
                kind == SKYRIM_PERF_GPU_COMMAND_BUFFER,
            (unsigned long long)w.failures, (unsigned long long)w.sleep_calls);
        if (used < 0 || (size_t)used >= sizeof(line)) continue;
        for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i) {
            int n = snprintf(line + used, sizeof(line) - (size_t)used, "%s%llu",
                i ? "," : "", (unsigned long long)w.buckets[i]);
            if (n < 0 || (size_t)n >= sizeof(line) - (size_t)used) { used = -1; break; }
            used += n;
        }
        if (attributed && used >= 0) {
            int n = snprintf(line + used, sizeof(line) - (size_t)used,
                " role=%u source=%u result=%u", context.role, context.source, context.result);
            if (n < 0 || (size_t)n >= sizeof(line) - (size_t)used) used = -1;
            else used += n;
        }
        if (used >= 0 && (size_t)used + 2 <= sizeof(line)) {
            line[used++] = '\n'; line[used] = 0; fputs(line, stderr);
        }
    }
    log_shader_waits();
    SkyrimPerfStalls stalls;
    if (skyrim_perf_stalls_take(&stalls)) {
        /* This is an approximate recorder window, not the endpoints of every
         * operation: a long interval can begin before its completion window.
         * Advance it even for an empty successful drain; a busy drain retains
         * the prior boundary because no samples/counters were consumed. */
        uint64_t stall_end_ns = skyrim_perf_now_ns();
        uint64_t stall_start_ns = stall_end_ns >= previous_stall_end_ns ? previous_stall_end_ns : 0;
        previous_stall_end_ns = stall_end_ns;
        if (!stalls.count && !stalls.dropped_busy && !stalls.dropped_full) return;
        /* Formatting and file I/O are outside the recording lock, at the same
         * five-second reporting cadence. Numeric spans contain no game data. */
        char line[8192];
        int used = snprintf(line, sizeof(line),
            "IOS_STALL schema=3 threshold_ns=%llu count=%u dropped_busy=%llu dropped_full=%llu window_start_ns=%llu window_end_ns=%llu fields=kind,thread,start_ns,end_ns,failed,estimated_end,role,source,result spans=",
            (unsigned long long)SKYRIM_PERF_STALL_THRESHOLD_NS, stalls.count,
            (unsigned long long)stalls.dropped_busy, (unsigned long long)stalls.dropped_full,
            (unsigned long long)stall_start_ns, (unsigned long long)stall_end_ns);
        if (used < 0 || (size_t)used >= sizeof(line)) return;
        for (unsigned i = 0; i < stalls.count; ++i) {
            const SkyrimPerfStall *s = &stalls.spans[i];
            int n = snprintf(line + used, sizeof(line) - (size_t)used, "%s%u,%llu,%llu,%llu,%u,%u,%u,%u,%u",
                i ? ";" : "", s->kind, (unsigned long long)s->thread,
                (unsigned long long)s->started_ns, (unsigned long long)s->ended_ns,
                s->failed, s->estimated_end, s->context.role, s->context.source, s->context.result);
            if (n < 0 || (size_t)n >= sizeof(line) - (size_t)used) { used = -1; break; }
            used += n;
        }
        if (used >= 0 && (size_t)used + 2 <= sizeof(line)) {
            line[used++] = '\n'; line[used] = 0; fputs(line, stderr);
        }
    }
}
