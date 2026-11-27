/* SPDX-License-Identifier: GPL-3.0-only */
#include "ios_runtime.h"
#include "mobile_core.h"
#include "benchmark.h"
#include "mobile_ui.h"
#include "performance_metrics.h"
#include "device_policy.h"
#include "display_timing.h"
#include "present_pacing_core.h"
#include "lifecycle_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

/* The guest main thread is the UIKit main thread. Loading can Present on a
 * worker while the main thread sleeps; keep UIKit alive without moving any
 * guest work or SDL input handlers onto a new thread. */
static SDL_Window *ui_window;
static uint64_t drawable_size;
static uint64_t submitted_render_size;
static uint64_t last_pump_ns;
static uint64_t last_phase_ns;
static unsigned present_phase;
static unsigned present_on_main;
static unsigned pumping; /* main-thread-only reentrancy guard */
static uint64_t driver_count, driver_total, driver_max, interval_max, last_frame;
/* Only diagnostic state is protected. No rendering or event work runs while
 * locked. The first frame after resuming is not a multi-minute frame stall. */
static pthread_mutex_t frame_metrics_mutex = PTHREAD_MUTEX_INITIALIZER;
static int metrics_active = 1;
static uint64_t metrics_epoch;

void skyrim_ios_metrics_active(int active) {
    skyrim_perf_activity(active);
    pthread_mutex_lock(&frame_metrics_mutex);
    metrics_active = !!active;
    last_frame = 0;
    __atomic_add_fetch(&metrics_epoch, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&frame_metrics_mutex);
}

static void atomic_max(uint64_t *target, uint64_t value) {
    uint64_t old = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (value > old && !__atomic_compare_exchange_n(target, &old, value, 1,
                                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

static void update_drawable(void) {
    if (!ui_window) return;
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(ui_window, &w, &h);
    if (w <= 0 || h <= 0) return;
    uint64_t size = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    uint64_t old = __atomic_exchange_n(&drawable_size, size, __ATOMIC_RELEASE);
    if (old != size)
        fprintf(stderr, "IOS_DRAWABLE pixels=%dx%d main_thread=1\n", w, h);
}

void skyrim_ios_set_window(void *window) {
    if (!pthread_main_np()) { fprintf(stderr, "FATAL UIKit window off main thread\n"); abort(); }
    skyrim_lifecycle_initialize();
    ui_window = window;
    update_drawable();
    skyrim_mobile_install(window);
}

void skyrim_ios_pump_events(void) {
    if (!pthread_main_np() || !ui_window || pumping) return;
    uint64_t now = monotonic_ns();
    /* Sleep(0) can be extremely hot. Service UIKit at most once per 4 ms. */
    if (now - __atomic_load_n(&last_pump_ns, __ATOMIC_RELAXED) < UINT64_C(4000000)) return;
    uint64_t epoch = __atomic_load_n(&metrics_epoch, __ATOMIC_RELAXED);
    pumping = 1;
    __atomic_store_n(&last_pump_ns, now, __ATOMIC_RELAXED);
    SDL_PumpEvents();
    update_drawable();
    skyrim_mobile_tick();
    pumping = 0;
    /* This is outside every admitted D3D call. The guest main stack stays in
     * place while UIKit continues servicing notifications and user actions. */
    skyrim_lifecycle_main_checkpoint();
    int saved_errno = errno;
    if (epoch == __atomic_load_n(&metrics_epoch, __ATOMIC_RELAXED))
        skyrim_perf_interval(SKYRIM_PERF_UI_PUMP, now, monotonic_ns(), 0, 0);
    errno = saved_errno;
}

void skyrim_ios_drawable_size(int *width, int *height) {
    if (pthread_main_np()) update_drawable();
    uint64_t size = __atomic_load_n(&drawable_size, __ATOMIC_ACQUIRE);
    if (size) { *width = (int)(size >> 32); *height = (int)(uint32_t)size; }
}

int skyrim_ios_window_pixel_size(int *width, int *height) {
    if (!pthread_main_np() || !ui_window) return 0;
    /* Before Vulkan creates its Metal view, the drawable query returns
     * points. UIKit's pixel-size query already applies the window's DPI. */
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(ui_window, &w, &h);
    if (w <= 0 || h <= 0) return 0;
    *width = w;
    *height = h;
    return 1;
}

void skyrim_ios_render_submitted(unsigned width, unsigned height) {
    const uint64_t size = ((uint64_t)width << 32) | height;
    if (size != __atomic_load_n(&submitted_render_size, __ATOMIC_RELAXED))
        __atomic_store_n(&submitted_render_size, size, __ATOMIC_RELEASE);
}

int skyrim_ios_render_size_submitted(unsigned width, unsigned height) {
    return width && height &&
        __atomic_load_n(&submitted_render_size, __ATOMIC_ACQUIRE) == (((uint64_t)width << 32) | height);
}

void skyrim_ios_present_phase(unsigned phase) {
    static _Thread_local uint64_t entered;
    static _Thread_local uint64_t entered_epoch;
    uint64_t now = monotonic_ns();
    if (phase == 3) { entered = now; entered_epoch = __atomic_load_n(&metrics_epoch, __ATOMIC_RELAXED); }
    if (phase == 4 && entered && entered_epoch == __atomic_load_n(&metrics_epoch, __ATOMIC_RELAXED)) {
        uint64_t elapsed = now - entered;
        __atomic_add_fetch(&driver_count, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&driver_total, elapsed, __ATOMIC_RELAXED);
        atomic_max(&driver_max, elapsed);
        skyrim_perf_interval(SKYRIM_PERF_PRESENT_HOST, entered, now, 0, 0);
        entered = 0;
    }
    __atomic_store_n(&present_on_main, !!pthread_main_np(), __ATOMIC_RELAXED);
    __atomic_store_n(&last_phase_ns, monotonic_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&present_phase, phase, __ATOMIC_RELAXED);
}

void skyrim_ios_take_frame_metrics(uint64_t *count, uint64_t *total, uint64_t *maximum, uint64_t *interval) {
    /* Coarse counters can straddle the boundary by one concurrent Present.
     * They measure host time inside Present, not GPU execution time. */
    *count = __atomic_exchange_n(&driver_count, 0, __ATOMIC_RELAXED);
    *total = __atomic_exchange_n(&driver_total, 0, __ATOMIC_RELAXED);
    *maximum = __atomic_exchange_n(&driver_max, 0, __ATOMIC_RELAXED);
    *interval = __atomic_exchange_n(&interval_max, 0, __ATOMIC_RELAXED);
}

void skyrim_ios_pace_frame(void) {
    /* A loading thread may own Present temporarily. Do not share an unsafe
     * clock, replay missed frames or busy-spin. Keep UIKit alive while waiting. */
    static _Thread_local SkyrimFrameClock clock;
    uint64_t started = monotonic_ns(), sleeps = 0;
    uint64_t epoch = __atomic_load_n(&metrics_epoch, __ATOMIC_RELAXED);
    const unsigned fps = skyrim_mobile_fps_limit();
    uint64_t deadline = skyrim_mobile_frame_deadline(&clock, started, fps);
    if (skyrim_present_sync_enabled()) {
        /* Opt-in: Metal spaces the frames by the cap, so the CPU only keeps the game from
         * running far ahead and releases half a period early. Off: this block never runs. */
        const uint64_t period = skyrim_present_min_duration_ns(fps);
        skyrim_present_set_target(period);
        skyrim_present_refresh_hint(fps);
        deadline = skyrim_present_cpu_deadline(deadline, started, period, 1);
    }
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now >= deadline) break;
        uint64_t remaining = deadline - now;
        sleeps += skyrim_device_platform_wait(remaining);
        skyrim_ios_pump_events();
    }
    int saved_errno = errno;
    uint64_t ended = monotonic_ns();
    if (epoch == __atomic_load_n(&metrics_epoch, __ATOMIC_RELAXED)) {
        skyrim_perf_interval(SKYRIM_PERF_PACER, started, ended, 0, sleeps);
        /* Excess wall time after a requested deadline. This overlaps the
         * run-loop/sleep/pump spans; it is not extra work or CPU time. */
        if (deadline > started && ended > deadline)
            skyrim_perf_interval(SKYRIM_PERF_PACER_OVERRUN, deadline, ended, 0, 0);
    }
    errno = saved_errno;
}

void skyrim_ios_watchdog(void) {
    uint64_t now = monotonic_ns();
    uint64_t phase = __atomic_load_n(&last_phase_ns, __ATOMIC_RELAXED);
    uint64_t pump = __atomic_load_n(&last_pump_ns, __ATOMIC_RELAXED);
    fprintf(stderr, "IOS_PROGRESS present_phase=%u present_main=%u phase_age_ms=%llu ui_pump_age_ms=%llu\n",
        __atomic_load_n(&present_phase, __ATOMIC_RELAXED),
        __atomic_load_n(&present_on_main, __ATOMIC_RELAXED),
        (unsigned long long)(phase && now >= phase ? (now - phase) / 1000000 : 0),
        (unsigned long long)(pump && now >= pump ? (now - pump) / 1000000 : 0));
}

void skyrim_ios_stage(const char *stage) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "IOS_STAGE %s monotonic=%lld.%09ld\n", stage,
            (long long)now.tv_sec, now.tv_nsec);
    fflush(stderr);
}

void skyrim_ios_present_result(int32_t result) {
    static unsigned successful_presents;
    static int32_t last_error;
    if (result < 0) {
        if (result != __atomic_exchange_n(&last_error, result, __ATOMIC_RELAXED))
            fprintf(stderr, "IOS_PRESENT failure=0x%08x\n", (uint32_t)result);
        fflush(stderr);
        return;
    }
    __atomic_store_n(&last_error, 0, __ATOMIC_RELAXED);
    pthread_mutex_lock(&frame_metrics_mutex);
    uint64_t now = monotonic_ns(), gap = 0;
    if (metrics_active) {
        if (last_frame && now >= last_frame) gap = now - last_frame;
        last_frame = now;
    }
    pthread_mutex_unlock(&frame_metrics_mutex);
    if (gap) {
        atomic_max(&interval_max, gap);
        skyrim_perf_interval(SKYRIM_PERF_FRAME_GAP, now - gap, now, 0, 0);
    }
    skyrim_benchmark_frame(now);
    skyrim_mobile_frame();
    if (__atomic_add_fetch(&successful_presents, 1, __ATOMIC_RELAXED) == 1)
        skyrim_ios_stage("first-successful-present-title-not-verified");
}
