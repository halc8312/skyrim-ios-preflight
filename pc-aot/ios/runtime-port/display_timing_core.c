/* SPDX-License-Identifier: GPL-3.0-only */
#include "display_timing_core.h"
#include "performance_metrics.h"
#include <stdatomic.h>

/* All state is relaxed atomics. Presented callbacks for successive drawables
 * are normally serialized by Metal, but nothing here depends on that: a value
 * that arrives out of order is dropped rather than recorded as a negative gap. */
static _Atomic uint64_t last_presented_media_ns;
static _Atomic uint64_t gpu_busy_ns;
#define NONE UINT64_MAX
static _Atomic uint64_t gpu_first_start_ns = NONE;   /* NONE: no buffer yet */
static _Atomic uint64_t gpu_last_end_ns;
static _Atomic uint64_t gpu_buffers;

uint64_t skyrim_display_to_host_ns(uint64_t media_ns, uint64_t now_media_ns, uint64_t now_host_ns) {
    if (!media_ns || !now_host_ns || media_ns > now_media_ns) return 0;
    const uint64_t age = now_media_ns - media_ns;
    return age < now_host_ns ? now_host_ns - age : 0;
}

void skyrim_display_reset(void) {
    atomic_store_explicit(&last_presented_media_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&gpu_busy_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&gpu_first_start_ns, NONE, memory_order_relaxed);
    atomic_store_explicit(&gpu_last_end_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&gpu_buffers, 0, memory_order_relaxed);
}

static void atomic_min(_Atomic uint64_t *slot, uint64_t value) {
    uint64_t old = atomic_load_explicit(slot, memory_order_relaxed);
    while (value < old && !atomic_compare_exchange_weak_explicit(
            slot, &old, value, memory_order_relaxed, memory_order_relaxed)) {}
}
static void atomic_max(_Atomic uint64_t *slot, uint64_t value) {
    uint64_t old = atomic_load_explicit(slot, memory_order_relaxed);
    while (value > old && !atomic_compare_exchange_weak_explicit(
            slot, &old, value, memory_order_relaxed, memory_order_relaxed)) {}
}

void skyrim_display_gpu_completed(uint64_t start, uint64_t end, int ok) {
    if (!ok || !start || end < start) {
        /* Counted, never folded into the totals: a failed buffer has no span. */
        skyrim_perf_observe(SKYRIM_PERF_GPU_COMMAND_BUFFER, 0, 1);
        return;
    }
    skyrim_perf_observe(SKYRIM_PERF_GPU_COMMAND_BUFFER, end - start, 0);
    atomic_fetch_add_explicit(&gpu_busy_ns, end - start, memory_order_relaxed);
    atomic_fetch_add_explicit(&gpu_buffers, 1, memory_order_relaxed);
    atomic_min(&gpu_first_start_ns, start);
    atomic_max(&gpu_last_end_ns, end);
}

void skyrim_display_presented(uint64_t presented, uint64_t acquired,
                              uint64_t now_media, uint64_t now_host) {
    if (!presented) {
        skyrim_perf_observe(SKYRIM_PERF_DISPLAY_DROPPED, 0, 0);
        return;
    }
    const uint64_t previous = atomic_exchange_explicit(&last_presented_media_ns, presented,
                                                      memory_order_relaxed);
    if (previous && presented > previous) {
        const uint64_t host_end = skyrim_display_to_host_ns(presented, now_media, now_host);
        const uint64_t host_start = skyrim_display_to_host_ns(previous, now_media, now_host);
        if (host_start && host_end > host_start)
            skyrim_perf_observe_span(SKYRIM_PERF_DISPLAY_INTERVAL, host_start, host_end);
        else
            skyrim_perf_observe(SKYRIM_PERF_DISPLAY_INTERVAL, presented - previous, 0);
    } else if (previous && presented < previous) {
        /* Out of order: keep the newer value as the reference, record nothing. */
        atomic_store_explicit(&last_presented_media_ns, previous, memory_order_relaxed);
    }
    if (acquired && presented >= acquired)
        skyrim_perf_observe(SKYRIM_PERF_DISPLAY_LATENCY, presented - acquired, 0);

    /* GPU work that completed since the previous presented callback. This is a
     * statistical pairing with the displayed frame, not an exact one: Metal does
     * not say which command buffers built which drawable. */
    const uint64_t buffers = atomic_exchange_explicit(&gpu_buffers, 0, memory_order_relaxed);
    const uint64_t busy = atomic_exchange_explicit(&gpu_busy_ns, 0, memory_order_relaxed);
    const uint64_t first = atomic_exchange_explicit(&gpu_first_start_ns, NONE, memory_order_relaxed);
    const uint64_t last = atomic_exchange_explicit(&gpu_last_end_ns, 0, memory_order_relaxed);
    if (buffers && first != NONE && last >= first) {
        skyrim_perf_observe(SKYRIM_PERF_GPU_FRAME_BUSY, busy, 0);
        skyrim_perf_observe(SKYRIM_PERF_GPU_FRAME_SPAN, last - first, 0);
    }
}
