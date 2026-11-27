/* SPDX-License-Identifier: GPL-3.0-only */
#include "benchmark.h"
#include <assert.h>

#define MS UINT64_C(1000000)
#define SECOND (1000 * MS)
#define COUNTDOWN (SKYRIM_BENCH_COUNTDOWN_SECONDS * SECOND)
#define DURATION (SKYRIM_BENCH_DURATION_SECONDS * SECOND)

static SkyrimBenchmarkResult take_result(void) {
    SkyrimBenchmarkResult result;
    assert(!skyrim_benchmark_active());
    assert(skyrim_benchmark_take_result(&result));
    assert(!skyrim_benchmark_take_result(&result));
    return result;
}

static void expect_triggers(uint64_t now, unsigned left, unsigned right) {
    SkyrimMobilePad pad = {0};
    assert(skyrim_benchmark_pad(now, &pad));
    assert(pad.lt == left && pad.rt == right);
    assert(!pad.buttons && !pad.lx && !pad.ly && !pad.rx && !pad.ry);
}

static void magic_press_and_release(void) {
    const uint64_t now = 10 * SECOND, start = now + COUNTDOWN;
    const uint64_t end = start + DURATION;
    SkyrimMobilePad pad = {.rt = 7};
    assert(skyrim_benchmark_start(SKYRIM_BENCH_MAGIC, now, 720, 60, 0, 0));
    assert(skyrim_benchmark_tick(now, 60, 0) == SKYRIM_BENCH_COUNTDOWN_SECONDS);
    assert(skyrim_benchmark_tick(start - 1, 60, 0) == 1);
    assert(!skyrim_benchmark_pad(start - 1, &pad) && pad.rt == 7);
    assert(skyrim_benchmark_tick(start, 60, 0) == 0);

    expect_triggers(start, 0, 0);
    expect_triggers(start + 2000 * MS - 1, 0, 0);
    expect_triggers(start + 2000 * MS, 255, 0);
    expect_triggers(start + 3300 * MS - 1, 255, 0);
    expect_triggers(start + 3300 * MS, 0, 0);
    expect_triggers(start + 7000 * MS - 1, 0, 0);
    expect_triggers(start + 7000 * MS, 255, 255);
    expect_triggers(start + 8300 * MS - 1, 255, 255);
    expect_triggers(start + 8300 * MS, 0, 0);
    expect_triggers(start + 12000 * MS, 255, 255);
    expect_triggers(start + 13300 * MS, 0, 0);
    expect_triggers(end, 0, 0);

    skyrim_benchmark_frame(start);
    skyrim_benchmark_frame(end);
    const SkyrimBenchmarkResult result = take_result();
    assert(result.status == SKYRIM_BENCH_COMPLETE && result.seen == 0x7f);
    const unsigned expected[] = {0, 2000, 3300, 7000, 8300, 12000, 13300};
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; ++i)
        assert(result.phase_ms[i] == expected[i]);
    assert(!skyrim_benchmark_pad(end + 1, &pad));
}

static void frame_threshold_and_end_crossing(void) {
    const uint64_t now = 100 * SECOND, start = now + COUNTDOWN;
    const uint64_t end = start + DURATION;
    skyrim_benchmark_render_size(1564, 720);
    assert(skyrim_benchmark_start(SKYRIM_BENCH_STILL, now, 720, 60, 0, 0));
    skyrim_benchmark_render_size(1564, 720);
    expect_triggers(start, 0, 0);
    skyrim_benchmark_frame(start - SECOND);
    skyrim_benchmark_frame(start);
    skyrim_benchmark_frame(start + 100 * MS);     /* exactly 100 ms */
    skyrim_benchmark_frame(start + 200 * MS + 1); /* 100 ms plus 1 ns */
    skyrim_benchmark_frame(start + 200 * MS);     /* stale present */
    for (uint64_t frame = start + 210 * MS; frame <= end - 100 * MS; frame += 10 * MS)
        skyrim_benchmark_frame(frame);
    skyrim_benchmark_frame(end + 350 * MS);       /* one 450 ms interval */

    const SkyrimBenchmarkResult result = take_result();
    assert(result.status == SKYRIM_BENCH_COMPLETE && result.intervals == 1973);
    assert(result.over100 == 2 && result.max_ns == 450 * MS && result.p99_ms == 10);
    assert(result.total_ns == DURATION + 350 * MS);
    assert(result.started_ns == start && result.finished_ns == end + 350 * MS);
    assert(result.width == 1564 && result.height == 720 && result.selected_height == 720);
}

static void cancel_timeout_and_restart(void) {
    const uint64_t now = 200 * SECOND, start = now + COUNTDOWN;
    SkyrimMobilePad pad = {0};
    assert(!skyrim_benchmark_start(0, now, 0, 0, 0, 0));
    assert(!skyrim_benchmark_start(SKYRIM_BENCH_LAST + 1, now, 0, 0, 0, 0));
    assert(!skyrim_benchmark_start(SKYRIM_BENCH_MOVE, UINT64_MAX, 0, 0, 0, 0));
    assert(skyrim_benchmark_start(SKYRIM_BENCH_MOVE, now, 0, 30, 0, 1));
    assert(!skyrim_benchmark_start(SKYRIM_BENCH_STILL, now, 0, 30, 0, 1));
    assert(skyrim_benchmark_pad(start + 3 * SECOND, &pad) && pad.ly > 0);
    skyrim_benchmark_cancel(start + 4 * SECOND);
    assert(!skyrim_benchmark_pad(start + 4 * SECOND, &pad));
    assert(!skyrim_benchmark_start(SKYRIM_BENCH_STILL, start + 4 * SECOND, 0, 30, 0, 1));
    const SkyrimBenchmarkResult cancelled = take_result();
    assert(cancelled.status == SKYRIM_BENCH_INTERRUPTED && cancelled.low_power == 1);

    const uint64_t restart = 300 * SECOND;
    const uint64_t timeout = restart + COUNTDOWN + DURATION + 15 * SECOND;
    assert(skyrim_benchmark_start(SKYRIM_BENCH_STILL, restart, 0, 60, 1, 0));
    assert(skyrim_benchmark_tick(timeout - 2, 40, 3) == 0);
    assert(skyrim_benchmark_tick(timeout - 1, 30, 2) == 0);
    assert(skyrim_benchmark_active());
    assert(skyrim_benchmark_tick(timeout, 30, 2) == 0);
    const SkyrimBenchmarkResult incomplete = take_result();
    assert(incomplete.run == cancelled.run + 1 && incomplete.scene == SKYRIM_BENCH_STILL);
    assert(incomplete.status == SKYRIM_BENCH_INCOMPLETE && incomplete.finished_ns == timeout);
    assert(incomplete.seen == 0 && incomplete.intervals == 0 && incomplete.low_power == 0);
    assert(incomplete.thermal_start == 1 && incomplete.thermal_max == 3);
    assert(incomplete.cap_start == 60 && incomplete.cap_end == 30);
}

static void resize_and_unobserved_phase(void) {
    const uint64_t now = 400 * SECOND, start = now + COUNTDOWN;
    assert(skyrim_benchmark_start(SKYRIM_BENCH_STILL, now, 720, 60, 0, 0));
    expect_triggers(start, 0, 0);
    skyrim_benchmark_frame(start);
    skyrim_benchmark_render_size(1920, 1080);
    skyrim_benchmark_render_size(1564, 720);
    skyrim_benchmark_frame(start + DURATION);
    const SkyrimBenchmarkResult resized = take_result();
    assert(resized.status == SKYRIM_BENCH_RESIZED && resized.width == 1564 && resized.height == 720);

    const uint64_t again = 500 * SECOND, again_start = again + COUNTDOWN;
    assert(skyrim_benchmark_start(SKYRIM_BENCH_TURN, again, 720, 60, 0, 0));
    expect_triggers(again_start, 0, 0);
    skyrim_benchmark_frame(again_start);
    skyrim_benchmark_frame(again_start + DURATION);
    const SkyrimBenchmarkResult missed = take_result();
    assert(missed.status == SKYRIM_BENCH_INCOMPLETE && missed.intervals == 1 && missed.seen == 1);
}

int main(void) {
    magic_press_and_release();
    frame_threshold_and_end_crossing();
    cancel_timeout_and_restart();
    resize_and_unobserved_phase();
    return 0;
}
