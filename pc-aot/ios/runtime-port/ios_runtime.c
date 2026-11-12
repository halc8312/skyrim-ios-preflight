/* SPDX-License-Identifier: GPL-3.0-only */
#include "ios_runtime.h"
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
    pumping = 0;
}

void skyrim_ios_drawable_size(int *width, int *height) {
    if (pthread_main_np()) update_drawable();
    uint64_t size = __atomic_load_n(&drawable_size, __ATOMIC_ACQUIRE);
    if (size) { *width = (int)(size >> 32); *height = (int)(uint32_t)size; }
}

void skyrim_ios_present_phase(unsigned phase) {
    __atomic_store_n(&present_on_main, !!pthread_main_np(), __ATOMIC_RELAXED);
    __atomic_store_n(&last_phase_ns, monotonic_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&present_phase, phase, __ATOMIC_RELAXED);
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
    if (__atomic_add_fetch(&successful_presents, 1, __ATOMIC_RELAXED) == 1)
        skyrim_ios_stage("first-successful-present-title-not-verified");
}
