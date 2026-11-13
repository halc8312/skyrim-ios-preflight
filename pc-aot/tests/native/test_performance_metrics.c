/* SPDX-License-Identifier: GPL-3.0-only */
#include "performance_metrics.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static _Atomic unsigned finished;
static void *produce(void *unused) {
    (void)unused;
    for (unsigned n = 0; n < 20000; ++n)
        skyrim_perf_record(SKYRIM_PERF_GRAPHICS_PIPELINE, 1234567, n % 4 == 0, 3);
    atomic_fetch_add(&finished, 1);
    return NULL;
}
static uint64_t count(const SkyrimPerfWindow *w) {
    uint64_t n = 0;
    for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i) n += w->buckets[i];
    return n;
}
int main(void) {
    SkyrimPerfWindow w;
    /* Exact endpoint and just-over-endpoint classification, including >2 s. */
    for (unsigned i = 0; i + 1 < SKYRIM_PERF_BUCKET_COUNT; ++i) {
        uint64_t limit = skyrim_perf_bucket_upper_ns(i);
        skyrim_perf_record(SKYRIM_PERF_FRAME_GAP, limit, 0, 0);
        skyrim_perf_take(SKYRIM_PERF_FRAME_GAP, &w);
        assert(count(&w) == 1 && w.buckets[i] == 1 && w.total_ns == limit && w.max_ns == limit);
        skyrim_perf_record(SKYRIM_PERF_FRAME_GAP, limit + 1, 0, 0);
        skyrim_perf_take(SKYRIM_PERF_FRAME_GAP, &w);
        assert(count(&w) == 1 && w.buckets[i + 1] == 1 && w.total_ns == limit + 1);
    }
    skyrim_perf_record(SKYRIM_PERF_COMPUTE_PIPELINE, 5000, 1, 0);
    skyrim_perf_take(SKYRIM_PERF_COMPUTE_PIPELINE, &w);
    assert(count(&w) == 1 && w.failures == 1 && w.max_ns == 5000);
    skyrim_perf_take(SKYRIM_PERF_COMPUTE_PIPELINE, &w);
    assert(count(&w) == 0 && w.failures == 0 && w.max_ns == 0);
    skyrim_perf_record(SKYRIM_PERF_KIND_COUNT, 7, 1, 3);
    skyrim_perf_end(SKYRIM_PERF_HLSL, 0, 0);
    skyrim_perf_take(SKYRIM_PERF_HLSL, &w);
    assert(count(&w) == 0);
    assert(skyrim_perf_now_ns() != 0);

    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, produce, NULL));
    uint64_t samples = 0, total = 0, failures = 0, sleeps = 0;
    do {
        skyrim_perf_take(SKYRIM_PERF_GRAPHICS_PIPELINE, &w);
        samples += count(&w); total += w.total_ns;
        failures += w.failures; sleeps += w.sleep_calls;
    } while (atomic_load(&finished) != 4);
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    skyrim_perf_take(SKYRIM_PERF_GRAPHICS_PIPELINE, &w);
    samples += count(&w); total += w.total_ns;
    failures += w.failures; sleeps += w.sleep_calls;
    assert(samples == 80000 && total == 80000 * UINT64_C(1234567));
    assert(failures == 20000 && sleeps == 240000);
    skyrim_perf_record(SKYRIM_PERF_PACER, 33000000, 0, 8);
    skyrim_perf_record(SKYRIM_PERF_GRAPHICS_COMPILE_WAIT, 120000000, 0, 0);
    skyrim_perf_log_window();
    puts("performance_histogram_boundaries_and_concurrent_drain=PASS");
    return 0;
}
