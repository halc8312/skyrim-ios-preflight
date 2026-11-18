/* Authored intervals/clock only. Exercise production recording, not game data. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static _Atomic uint64_t fixture_now;
static _Atomic unsigned clock_calls;
static int fixture_clock_gettime(clockid_t clock, struct timespec *out) {
    assert(clock == CLOCK_MONOTONIC);
    atomic_fetch_add(&clock_calls, 1);
    uint64_t now = atomic_load(&fixture_now);
    errno = EIO;
    if (!now) return -1;
    out->tv_sec = (time_t)(now / 1000000000);
    out->tv_nsec = (long)(now % 1000000000);
    return 0;
}
#define clock_gettime fixture_clock_gettime
#include "performance_metrics.c"
#undef clock_gettime

static _Atomic unsigned finished;
static uint64_t worker_tags[4];
static void *produce(void *arg) {
    size_t n = (size_t)arg;
    for (unsigned i = 0; i < 20000; ++i)
        skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 5000000000 + i, 5100000000 + i, n & 1, 0);
    worker_tags[n] = thread_ordinal;
    atomic_fetch_add(&finished, 1);
    return NULL;
}
static void clear_histograms(void) {
    for (unsigned kind = 0; kind < SKYRIM_PERF_KIND_COUNT; ++kind) {
        SkyrimPerfWindow w; skyrim_perf_take(kind, &w);
    }
    /* The recorder also retains partitions of those same events. Drain both
     * sets when isolating a later exact-output fixture. */
    for (unsigned index = 0; index < CONTEXT_COUNT; ++index) {
        SkyrimPerfContext context;
        unsigned kind = context_decode(index, &context);
        SkyrimPerfWindow w; skyrim_perf_context_take(kind, context, &w);
    }
}
static void verify_spans(const SkyrimPerfStalls *s) {
    assert(s->count <= SKYRIM_PERF_STALL_CAPACITY);
    for (unsigned i = 0; i < s->count; ++i) {
        const SkyrimPerfStall *p = &s->spans[i];
        assert(p->kind < SKYRIM_PERF_KIND_COUNT && p->failed <= 1 && p->estimated_end <= 1);
        assert(p->started_ns && p->ended_ns >= p->started_ns + SKYRIM_PERF_STALL_THRESHOLD_NS);
    }
}

static void *produce_shader_waits(void *arg) {
    const uint64_t base = UINT64_C(5000000000) + (size_t)arg;
    const SkyrimPerfPrefetch job = {base, base + 1, base + 2};
    assert(!skyrim_perf_prefetch_set(job).queued_ns);
    skyrim_perf_context_set((SkyrimPerfContext){SKYRIM_PERF_ROLE_DRAW, 0, 0});
    for (unsigned i = 0; i < 4000; ++i) {
        SkyrimPerfShaderOrigin origin = {skyrim_perf_prefetch_get(), base + 3, base + 4};
        skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, base, base + SKYRIM_PERF_STALL_THRESHOLD_NS, origin);
    }
    assert(skyrim_perf_prefetch_get().queued_ns == job.queued_ns);
    atomic_fetch_add(&finished, 1);
    return NULL;
}

static void verify_shader_waits(void) {
    const uint64_t start = UINT64_C(5300000000), end = start + SKYRIM_PERF_STALL_THRESHOLD_NS;
    const SkyrimPerfShaderOrigin known = {
        {UINT64_C(5000000000), UINT64_C(5100000000), UINT64_C(5200000000)}, start, end
    };
    ShaderWaitBatch batch;
    skyrim_perf_context_set((SkyrimPerfContext){SKYRIM_PERF_ROLE_PREFETCH, 0, 0});
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end, known);
    skyrim_perf_context_set((SkyrimPerfContext){SKYRIM_PERF_ROLE_DRAW, 0, 0});
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_UNKNOWN, start, end, known);
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end - 1, known);
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, end, start, known);
    skyrim_perf_activity(0);
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end, known);
    skyrim_perf_activity(1); /* mock clock at 4 s */
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, 3999999999ULL, end, known);
    assert(shader_waits_take(&batch) && batch.count == 0);

    const unsigned clock_count = atomic_load(&clock_calls);
    SkyrimPerfShaderOrigin partial = known;
    partial.prefetch.ready_ns = 3000000000ULL; /* before the active interval */
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end, partial);
    partial = known;
    partial.prefetch.queued_ns = 3000000000ULL;
    partial.prefetch.ready_ns = 0;
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end, partial);
    partial = known;
    partial.published_ns = end + 1;
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end, partial);
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_CREATE, start, end, known);
    assert(shader_waits_take(&batch) && batch.count == 4);
    assert(!batch.waits[0].origin.prefetch.ready_ns && batch.waits[0].origin.prefetch.queued_ns == known.prefetch.queued_ns);
    for (unsigned i = 1; i < batch.count; ++i) {
        assert(!batch.waits[i].origin.prefetch.queued_ns && !batch.waits[i].origin.published_ns);
    }
    assert(batch.waits[3].result == SKYRIM_PERF_WAIT_CREATE && atomic_load(&clock_calls) == clock_count);

    for (unsigned i = 0; i < SHADER_WAIT_CAPACITY + 1; ++i)
        skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end, known);
    assert(shader_waits_take(&batch) && batch.count == SHADER_WAIT_CAPACITY && batch.full == 1 && !batch.busy);

    pthread_t workers[4];
    atomic_store(&finished, 0);
    for (size_t i = 0; i < 4; ++i) assert(!pthread_create(&workers[i], NULL, produce_shader_waits, (void *)i));
    uint64_t observed = 0;
    do {
        if (shader_waits_take(&batch)) {
            observed += batch.count + batch.busy + batch.full;
            for (unsigned i = 0; i < batch.count; ++i) {
                const ShaderWait *wait = &batch.waits[i];
                assert(wait->origin.prefetch.worker_ns == wait->wait_start_ns + 2);
                assert(wait->origin.published_ns == wait->wait_start_ns + 4);
            }
        }
    } while (atomic_load(&finished) < 4);
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
    assert(shader_waits_take(&batch) && observed + batch.count + batch.busy + batch.full == 16000);
    assert(!skyrim_perf_prefetch_get().queued_ns);

    assert(!atomic_flag_test_and_set(&shader_wait_lock));
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end, known);
    atomic_flag_clear(&shader_wait_lock);
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_HIT, start, end, known);
    skyrim_perf_shader_wait(SKYRIM_PERF_WAIT_CREATE, start, end, known);
    atomic_store(&fixture_now, end);
    skyrim_perf_log_window(); /* no ordinary timing or stall rows in this window */
    puts("shader_waits_foreground_origin_and_concurrent_accounting=PASS");
}

int main(void) {
    SkyrimPerfStalls s;
    atomic_store(&fixture_now, 1000000000);
    errno = EINTR;
    skyrim_perf_interval(SKYRIM_PERF_HLSL, 100, 49999999 + 100, 0, 0);
    skyrim_perf_record(SKYRIM_PERF_HLSL, 49999999, 0, 0);
    assert(clock_calls == 0 && errno == EINTR);
    assert(skyrim_perf_stalls_take(&s) && !s.count);
    skyrim_perf_interval(SKYRIM_PERF_HLSL, 100, 50000000 + 100, 3, 0);
    assert(clock_calls == 0);
    assert(skyrim_perf_stalls_take(&s) && s.count == 1);
    assert(s.spans[0].started_ns == 100 && s.spans[0].ended_ns == 50000100);
    assert(s.spans[0].failed == 1 && s.spans[0].thread == 1 && !s.spans[0].estimated_end);
    skyrim_perf_record(SKYRIM_PERF_TEXTURE_LOAD, 60000000, 0, 0);
    assert(clock_calls == 1 && errno == EINTR);
    assert(skyrim_perf_stalls_take(&s) && s.count == 1);
    assert(s.spans[0].started_ns == 940000000 && s.spans[0].ended_ns == 1000000000);
    assert(s.spans[0].estimated_end && s.spans[0].thread == 1);
    skyrim_perf_end(SKYRIM_PERF_GRAPHICS_PIPELINE, 900000000, 0);
    assert(clock_calls == 2 && errno == EINTR);
    assert(skyrim_perf_stalls_take(&s) && s.count == 1 && !s.spans[0].estimated_end);
    skyrim_perf_interval(SKYRIM_PERF_HLSL, 0, 100000000, 0, 0);
    skyrim_perf_interval(SKYRIM_PERF_HLSL, 100, 99, 0, 0);
    skyrim_perf_interval(SKYRIM_PERF_KIND_COUNT, 100, 100000000, 0, 0);
    skyrim_perf_record(SKYRIM_PERF_HLSL, UINT64_MAX, 0, 0);
    assert(skyrim_perf_stalls_take(&s) && !s.count);

    /* A representative nested compile burst exceeds the old 12-work-slot budget.
     * Retain all 19 work spans without consuming the frame reservation. */
    for (unsigned i = 0; i < 19; ++i)
        skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 100 + i, 100000100 + i, 0, 0);
    for (unsigned i = 0; i < 4; ++i)
        skyrim_perf_interval(SKYRIM_PERF_FRAME_GAP, 100 + i, 100000100 + i, 0, 0);
    assert(skyrim_perf_stalls_take(&s) && s.count == 23 && !s.dropped_full && !s.dropped_busy);
    verify_spans(&s);
    for (unsigned i = 0; i < SKYRIM_PERF_STALL_WORK + 2; ++i)
        skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 100 + i, 100000100 + i, 0, 0);
    for (unsigned i = 0; i < SKYRIM_PERF_STALL_FRAMES + 2; ++i)
        skyrim_perf_interval(SKYRIM_PERF_FRAME_GAP, 100 + i, 100000100 + i, 0, 0);
    assert(skyrim_perf_stalls_take(&s) && s.count == SKYRIM_PERF_STALL_CAPACITY && s.dropped_full == 4 && !s.dropped_busy);
    verify_spans(&s);
    for (unsigned i = 0; i < SKYRIM_PERF_STALL_CAPACITY; ++i) {
        assert(s.spans[i].kind == (i < SKYRIM_PERF_STALL_FRAMES ? SKYRIM_PERF_FRAME_GAP : SKYRIM_PERF_READ_FILE));
        assert(s.spans[i].started_ns == 100 + (i < SKYRIM_PERF_STALL_FRAMES ? i : i - SKYRIM_PERF_STALL_FRAMES));
    }
    assert(skyrim_perf_stalls_take(&s) && !s.count && !s.dropped_full);
    assert(!atomic_flag_test_and_set(&stall_lock));
    skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 100, 100000100, 0, 0);
    assert(!skyrim_perf_stalls_take(&s) && !s.count);
    atomic_flag_clear(&stall_lock);
    assert(skyrim_perf_stalls_take(&s) && s.dropped_busy == 1 && !s.count);

    skyrim_perf_activity(0);
    for (unsigned k = SKYRIM_PERF_PACER_RUNLOOP; k <= SKYRIM_PERF_PACER_OVERRUN; ++k) {
        SkyrimPerfWindow active;
        skyrim_perf_interval(k, 1000000000, 2100000000, 0, 0);
        skyrim_perf_take(k, &active);
        assert(!active.total_ns && !active.max_ns);
    }
    skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 1000000000, 2000000000, 0, 0);
    assert(skyrim_perf_stalls_take(&s) && !s.count);
    atomic_store(&fixture_now, 2000000000);
    skyrim_perf_activity(1);
    for (unsigned k = SKYRIM_PERF_PACER_RUNLOOP; k <= SKYRIM_PERF_PACER_OVERRUN; ++k) {
        SkyrimPerfWindow active;
        skyrim_perf_interval(k, 1999999999, 2100000000, 0, 0);
        skyrim_perf_take(k, &active);
        assert(!active.total_ns && !active.max_ns);
        skyrim_perf_interval(k, 2000000000, 2001000000, 0, 0);
        skyrim_perf_take(k, &active);
        assert(active.total_ns == 1000000 && active.max_ns == 1000000);
    }
    skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 1999999999, 2100000000, 0, 0);
    skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 2000000000, 2100000000, 0, 0);
    assert(skyrim_perf_stalls_take(&s) && s.count == 1 && s.spans[0].started_ns == 2000000000);
    atomic_store(&fixture_now, 0);
    skyrim_perf_activity(1); /* failed clock keeps samples disabled */
    skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 3000000000, 3100000000, 0, 0);
    assert(skyrim_perf_stalls_take(&s) && !s.count && errno == EINTR);
    atomic_store(&fixture_now, 4000000000);
    skyrim_perf_activity(1);
    clear_histograms();

    pthread_t workers[4];
    for (size_t n = 0; n < 4; ++n) assert(!pthread_create(&workers[n], NULL, produce, (void *)n));
    uint64_t admitted = 0, busy = 0, full = 0;
    do {
        if (skyrim_perf_stalls_take(&s)) {
            verify_spans(&s); admitted += s.count; busy += s.dropped_busy; full += s.dropped_full;
        }
    } while (atomic_load(&finished) < 4);
    for (size_t n = 0; n < 4; ++n) assert(!pthread_join(workers[n], NULL));
    assert(skyrim_perf_stalls_take(&s));
    admitted += s.count; busy += s.dropped_busy; full += s.dropped_full;
    assert(admitted + busy + full == 80000 && admitted > 0);
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < i; ++j)
        assert(!worker_tags[i] || !worker_tags[j] || worker_tags[i] != worker_tags[j]);
    SkyrimPerfWindow w; skyrim_perf_take(SKYRIM_PERF_READ_FILE, &w);
    assert(w.total_ns == UINT64_C(80000) * 100000000 && w.failures == 40000);
    clear_histograms();
    thread_ordinal = 0;
    atomic_store(&next_thread, UINT64_MAX);
    skyrim_perf_interval(SKYRIM_PERF_READ_FILE, 5000000000, 5100000000, 0, 0);
    assert(skyrim_perf_stalls_take(&s) && s.count == 1 && s.spans[0].thread == 0);
    assert(atomic_load(&next_thread) == UINT64_MAX);
    clear_histograms();
    skyrim_perf_interval(SKYRIM_PERF_FRAME_GAP, 5000000000, 5100000000, 0, 0);
    skyrim_perf_interval(SKYRIM_PERF_HLSL, 5010000000, 5090000000, 1, 0);
    skyrim_perf_log_window();
    assert(errno == EINTR);
    assert(skyrim_perf_stalls_take(&s) && !s.count);
    puts("bounded_stalls_threshold_reservation_lifecycle_concurrent_accounting=PASS");
    verify_shader_waits();
    return 0;
}
