/* SPDX-License-Identifier: GPL-3.0-only */
/* Authored values only. Exercises the real recorder through the real core. */
#include "display_timing_core.h"
#include "performance_metrics.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#define MS UINT64_C(1000000)

static uint64_t count(const SkyrimPerfWindow *w) {
    uint64_t n = 0;
    for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i) n += w->buckets[i];
    return n;
}
static SkyrimPerfWindow take(unsigned kind) {
    SkyrimPerfWindow w;
    skyrim_perf_take(kind, &w);
    return w;
}
static void drain_all(void) {
    for (unsigned k = 0; k < SKYRIM_PERF_KIND_COUNT; ++k) (void)take(k);
    SkyrimPerfStalls stalls;
    (void)skyrim_perf_stalls_take(&stalls);
    skyrim_display_reset();
}
static unsigned bucket_of(uint64_t ns) {
    for (unsigned i = 0; i < SKYRIM_PERF_BUCKET_COUNT; ++i)
        if (ns <= skyrim_perf_bucket_upper_ns(i)) return i;
    return SKYRIM_PERF_BUCKET_COUNT - 1;
}

static void clock_mapping(void) {
    /* Media and host clocks have unrelated origins; only differences carry over. */
    assert(skyrim_display_to_host_ns(900, 1000, 50000) == 49900);
    assert(skyrim_display_to_host_ns(1000, 1000, 50000) == 50000);
    assert(skyrim_display_to_host_ns(0, 1000, 50000) == 0);       /* unavailable */
    assert(skyrim_display_to_host_ns(1001, 1000, 50000) == 0);    /* in the future */
    assert(skyrim_display_to_host_ns(1, 60000, 50000) == 0);      /* before host origin */
    assert(skyrim_display_to_host_ns(900, 1000, 0) == 0);         /* host clock failed */
}

static void display_intervals_and_drops(void) {
    drain_all();
    /* Three frames shown 16.667 ms apart, read 2 ms after the last one. */
    const uint64_t media = 7000 * MS, host = 90000 * MS, step = 16666667;
    skyrim_display_presented(media, media - 10 * MS, media + 1 * MS, host);
    skyrim_display_presented(media + step, media + step - 12 * MS, media + step + 2 * MS, host + step);
    skyrim_display_presented(media + 2 * step, media + 2 * step - 15 * MS, media + 2 * step + 2 * MS, host + 2 * step);
    SkyrimPerfWindow w = take(SKYRIM_PERF_DISPLAY_INTERVAL);
    assert(count(&w) == 2);                       /* first frame has no predecessor */
    assert(w.total_ns == 2 * step && w.max_ns == step);
    assert(w.buckets[bucket_of(step)] == 2);
    w = take(SKYRIM_PERF_DISPLAY_LATENCY);
    assert(count(&w) == 3 && w.total_ns == 37 * MS);

    /* A frame Metal reports as not shown (presentedTime 0) is counted, and does
     * not become the reference for the next interval. */
    skyrim_display_presented(0, media, media + 100 * MS, host + 100 * MS);
    w = take(SKYRIM_PERF_DISPLAY_DROPPED);
    assert(count(&w) == 1 && w.total_ns == 0);
    skyrim_display_presented(media + 3 * step, 0, media + 3 * step, host + 3 * step);
    w = take(SKYRIM_PERF_DISPLAY_INTERVAL);
    assert(count(&w) == 1 && w.total_ns == step);
    w = take(SKYRIM_PERF_DISPLAY_LATENCY);
    assert(count(&w) == 0);                       /* acquired == 0: unavailable */

    /* An out-of-order callback records nothing and keeps the newer reference. */
    skyrim_display_presented(media + 1 * step, 0, media + 4 * step, host + 4 * step);
    w = take(SKYRIM_PERF_DISPLAY_INTERVAL);
    assert(count(&w) == 0);
    skyrim_display_presented(media + 4 * step, 0, media + 4 * step, host + 4 * step);
    w = take(SKYRIM_PERF_DISPLAY_INTERVAL);
    assert(count(&w) == 1 && w.total_ns == step);
}

static void long_gap_becomes_one_bounded_span(void) {
    drain_all();
    const uint64_t media = 5000 * MS, host = 9000 * MS;
    skyrim_display_presented(media, 0, media, host);
    /* 99 ms: histogram only. 250 ms: histogram plus one stall sample. */
    skyrim_display_presented(media + 99 * MS, 0, media + 99 * MS, host + 99 * MS);
    SkyrimPerfStalls s;
    assert(skyrim_perf_stalls_take(&s) && s.count == 0);
    skyrim_display_presented(media + 349 * MS, 0, media + 349 * MS, host + 349 * MS);
    assert(skyrim_perf_stalls_take(&s) && s.count == 1);
    assert(s.spans[0].kind == SKYRIM_PERF_DISPLAY_INTERVAL);
    assert(s.spans[0].ended_ns == host + 349 * MS && s.spans[0].started_ns == host + 99 * MS);
    SkyrimPerfWindow w = take(SKYRIM_PERF_DISPLAY_INTERVAL);
    assert(count(&w) == 2 && w.max_ns == 250 * MS);
}

static void gpu_totals_pair_with_the_next_presented_callback(void) {
    drain_all();
    /* Two overlapping buffers: busy time adds up, span does not double-count. */
    skyrim_display_gpu_completed(1000 * MS, 1006 * MS, 1);
    skyrim_display_gpu_completed(1004 * MS, 1010 * MS, 1);
    skyrim_display_gpu_completed(1020 * MS, 1021 * MS, 1);
    skyrim_display_gpu_completed(1030 * MS, 1031 * MS, 0);    /* Metal error */
    skyrim_display_gpu_completed(0, 0, 1);                      /* never ran */
    skyrim_display_gpu_completed(1050 * MS, 1049 * MS, 1);      /* end before start */
    SkyrimPerfWindow w = take(SKYRIM_PERF_GPU_COMMAND_BUFFER);
    assert(count(&w) == 6 && w.failures == 3);
    assert(w.total_ns == (6 + 6 + 1) * MS);

    skyrim_display_presented(2000 * MS, 0, 2000 * MS, 70000 * MS);
    w = take(SKYRIM_PERF_GPU_FRAME_BUSY);
    assert(count(&w) == 1 && w.total_ns == 13 * MS);
    w = take(SKYRIM_PERF_GPU_FRAME_SPAN);
    assert(count(&w) == 1 && w.total_ns == 21 * MS);            /* 1000 -> 1021 */

    /* Totals were consumed: the next frame without buffers records nothing. */
    skyrim_display_presented(2016 * MS, 0, 2016 * MS, 70016 * MS);
    w = take(SKYRIM_PERF_GPU_FRAME_BUSY);
    assert(count(&w) == 0);
    w = take(SKYRIM_PERF_GPU_FRAME_SPAN);
    assert(count(&w) == 0);
}

static void reset_forgets_the_reference(void) {
    drain_all();
    skyrim_display_presented(100 * MS, 0, 100 * MS, 9000 * MS);
    skyrim_display_gpu_completed(10 * MS, 20 * MS, 1);
    skyrim_display_reset();   /* e.g. app went inactive: no interval across it */
    skyrim_display_presented(60000 * MS, 0, 60000 * MS, 70000 * MS);
    SkyrimPerfWindow w = take(SKYRIM_PERF_DISPLAY_INTERVAL);
    assert(count(&w) == 0);
    w = take(SKYRIM_PERF_GPU_FRAME_BUSY);
    assert(count(&w) == 0);
}

static void observers_accept_only_their_own_kinds(void) {
    drain_all();
    skyrim_perf_observe(SKYRIM_PERF_FRAME_GAP, 5 * MS, 0);
    skyrim_perf_observe(SKYRIM_PERF_KIND_COUNT, 5 * MS, 0);
    skyrim_perf_observe_span(SKYRIM_PERF_PACER, 1, 900 * MS);
    skyrim_perf_observe_span(SKYRIM_PERF_DISPLAY_INTERVAL, 0, 900 * MS);   /* no start */
    skyrim_perf_observe_span(SKYRIM_PERF_DISPLAY_INTERVAL, 900 * MS, 1 * MS); /* ends first */
    SkyrimPerfWindow w = take(SKYRIM_PERF_FRAME_GAP);
    assert(count(&w) == 0);
    w = take(SKYRIM_PERF_PACER);
    assert(count(&w) == 0);
    w = take(SKYRIM_PERF_DISPLAY_INTERVAL);
    assert(count(&w) == 0);
    SkyrimPerfStalls s;
    assert(skyrim_perf_stalls_take(&s) && s.count == 0);
}

static void *gpu_producer(void *arg) {
    (void)arg;
    for (unsigned n = 0; n < 50000; ++n)
        skyrim_display_gpu_completed((1 + n) * MS, (2 + n) * MS, 1);
    return NULL;
}
static void *present_producer(void *arg) {
    (void)arg;
    for (unsigned n = 0; n < 20000; ++n) {
        const uint64_t t = (1000 + n) * MS;
        skyrim_display_presented(t, t - MS, t + MS, t + 1000000 * MS);
    }
    return NULL;
}
static void concurrent_callbacks_do_not_crash_or_lose_buffers(void) {
    drain_all();
    pthread_t threads[3];
    pthread_create(&threads[0], NULL, gpu_producer, NULL);
    pthread_create(&threads[1], NULL, gpu_producer, NULL);
    pthread_create(&threads[2], NULL, present_producer, NULL);
    for (unsigned i = 0; i < 3; ++i) pthread_join(threads[i], NULL);
    SkyrimPerfWindow w = take(SKYRIM_PERF_GPU_COMMAND_BUFFER);
    assert(count(&w) == 100000);                   /* every buffer counted exactly once */
    assert(w.failures == 0 && w.total_ns == 100000 * MS);
    w = take(SKYRIM_PERF_DISPLAY_LATENCY);
    assert(count(&w) == 20000);
}

int main(void) {
    clock_mapping();
    display_intervals_and_drops();
    long_gap_becomes_one_bounded_span();
    gpu_totals_pair_with_the_next_presented_callback();
    reset_forgets_the_reference();
    observers_accept_only_their_own_kinds();
    concurrent_callbacks_do_not_crash_or_lose_buffers();
    /* Leave one of each kind for the reporter so the Python test can check the
     * exact lines the app would print: a failed buffer, a latency, a drop. */
    drain_all();
    skyrim_display_gpu_completed(0, 0, 1);
    skyrim_display_gpu_completed(10 * MS, 12 * MS, 1);
    skyrim_display_presented(0, 0, 0, 0);
    skyrim_display_presented(300 * MS, 290 * MS, 300 * MS, 9000 * MS);
    skyrim_perf_log_window();
    puts("display_timing=PASS");
    return 0;
}
