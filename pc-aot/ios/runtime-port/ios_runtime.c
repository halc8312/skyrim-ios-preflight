/* SPDX-License-Identifier: GPL-3.0-only */
#include "ios_runtime.h"
#include "mobile_core.h"
#include "mobile_ui.h"
#include "performance_metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

/* The guest main thread is the UIKit main thread. Loading can Present on a
 * worker while the main thread sleeps; keep UIKit alive without moving any
 * guest work or SDL input handlers onto a new thread. */
static SDL_Window *ui_window;
static uint64_t drawable_size;
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
    ui_window = window;
    update_drawable();
    skyrim_mobile_install(window);
}

void skyrim_ios_pump_events(void) {
    if (!pthread_main_np() || !ui_window || pumping) return;
    uint64_t now = monotonic_ns();
    /* Sleep(0) can be extremely hot. Service UIKit at most once per 4 ms. */
    if (now - __atomic_load_n(&last_pump_ns, __ATOMIC_RELAXED) < UINT64_C(4000000)) return;
    pumping = 1;
    __atomic_store_n(&last_pump_ns, now, __ATOMIC_RELAXED);
    SDL_PumpEvents();
    update_drawable();
    skyrim_mobile_tick();
    pumping = 0;
}

void skyrim_ios_drawable_size(int *width, int *height) {
    if (pthread_main_np()) update_drawable();
    uint64_t size = __atomic_load_n(&drawable_size, __ATOMIC_ACQUIRE);
    if (size) { *width = (int)(size >> 32); *height = (int)(uint32_t)size; }
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
        atomic_max(&driver_max, elapsed); entered = 0;
        skyrim_perf_record(SKYRIM_PERF_PRESENT_HOST, elapsed, 0, 0);
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
    uint64_t deadline = skyrim_mobile_frame_deadline(&clock, started, skyrim_mobile_fps_limit());
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now >= deadline) break;
        uint64_t remaining = deadline - now;
        struct timespec pause = {0, (long)(remaining > 4000000 ? 4000000 : remaining)};
        nanosleep(&pause, NULL);
        ++sleeps;
        skyrim_ios_pump_events();
    }
    if (epoch == __atomic_load_n(&metrics_epoch, __ATOMIC_RELAXED))
        skyrim_perf_record(SKYRIM_PERF_PACER, monotonic_ns() - started, 0, sleeps);
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
        skyrim_perf_record(SKYRIM_PERF_FRAME_GAP, gap, 0, 0);
    }
    skyrim_mobile_frame();
    if (__atomic_add_fetch(&successful_presents, 1, __ATOMIC_RELAXED) == 1)
        skyrim_ios_stage("first-successful-present-title-not-verified");
}
