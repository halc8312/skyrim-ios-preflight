/* SPDX-License-Identifier: GPL-3.0-only */
#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include "lifecycle_controller.h"
#include "lifecycle_runtime.h"

static SkyrimLifecycleController controller;
static dispatch_semaphore_t workerWake;
static CFRunLoopSourceRef mainWake;
static pthread_mutex_t waitMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t waitCondition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t registryMutex = PTHREAD_MUTEX_INITIALIZER;
static struct { void *device; SkyrimLifecycleDrain drain; } devices[8];
static _Thread_local unsigned hostDepth;
static uint64_t requestedAt;
static int initialized;

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}
static void lifecycle_log(unsigned event, uint64_t start, int32_t result) {
    const uint64_t now = now_ns();
    const SkyrimLifecycleSnapshot s = skyrim_lifecycle_controller_snapshot(&controller);
    fprintf(stderr, "IOS_LIFECYCLE schema=1 event=%u generation=%u phase=%u active_calls=%u requested=%u monotonic_ns=%llu elapsed_ns=%llu result=%d\n",
        event, s.generation, s.phase, s.active_calls, s.requested,
        (unsigned long long)now, (unsigned long long)(start && now >= start ? now - start : 0), result);
}
static void fatal(int32_t result, const char *message) {
    lifecycle_log(6, 0, result);
    fprintf(stderr, "FATAL lifecycle: %s\n", message); fflush(stderr); abort();
}
static void signal_main(void *unused) { (void)unused; }
static void wake_waiters(void) {
    pthread_cond_broadcast(&waitCondition);
    if (mainWake) {
        CFRunLoopSourceSignal(mainWake);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }
}
/* The predicate is atomic and UI never takes waitMutex. A timed worker wait
 * covers notification-before-wait without blocking UIKit to close that race.
 * Main has a permanent run-loop source and cannot spin on an empty run loop. */
static void wait_once(void) {
    if (pthread_main_np()) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    } else {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 50000000;
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
        pthread_mutex_lock(&waitMutex);
        pthread_cond_timedwait(&waitCondition, &waitMutex, &deadline);
        pthread_mutex_unlock(&waitMutex);
    }
}
static void *drain_worker(void *unused) {
    (void)unused;
    for (;;) { @autoreleasepool {
        dispatch_semaphore_wait(workerWake, DISPATCH_TIME_FOREVER);
        uint32_t generation = skyrim_lifecycle_controller_claim(&controller);
        if (!generation) continue;
        const uint64_t started = now_ns();
        lifecycle_log(2, started, 0);
        int32_t result = 0;
        pthread_mutex_lock(&registryMutex);
        /* Never wait for a consumer while another registered producer still
         * needs its flush. CS/submission workers are deliberately not gated. */
        for (unsigned phase = 0; phase < 3 && !result; ++phase)
            for (unsigned i = 0; i < 8 && !result; ++i)
                if (devices[i].device) result = devices[i].drain(devices[i].device, phase);
        pthread_mutex_unlock(&registryMutex);
        if (!skyrim_lifecycle_controller_complete(&controller, generation, result))
            fatal(SKYRIM_LIFECYCLE_INVALID_GENERATION, "invalid drain generation");
        lifecycle_log(result ? 6 : 3, started, result);
        wake_waiters();
    } }
    return NULL;
}
void skyrim_lifecycle_initialize(void) {
    if (!pthread_main_np()) fatal(SKYRIM_LIFECYCLE_INIT_OFF_MAIN, "initialization off main thread");
    if (initialized) return;
    skyrim_lifecycle_controller_init(&controller);
    workerWake = dispatch_semaphore_create(0);
    CFRunLoopSourceContext context = {0}; context.perform = signal_main;
    mainWake = CFRunLoopSourceCreate(NULL, 0, &context);
    if (!workerWake || !mainWake) fatal(SKYRIM_LIFECYCLE_NOTIFICATION_FAILED, "cannot create notification resources");
    CFRunLoopAddSource(CFRunLoopGetMain(), mainWake, kCFRunLoopDefaultMode);
    pthread_t worker;
    if (pthread_create(&worker, NULL, drain_worker, NULL)) fatal(SKYRIM_LIFECYCLE_WORKER_FAILED, "cannot start drain worker");
    pthread_detach(worker);
    initialized = 1;
}
void skyrim_lifecycle_request(int active) {
    if (!initialized) return; /* No game device exists before initialization. */
    const int saved = errno;
    if (skyrim_lifecycle_controller_request(&controller, active)) {
        if (!active) __atomic_store_n(&requestedAt, now_ns(), __ATOMIC_RELAXED);
        lifecycle_log(active ? 1 : 0, __atomic_load_n(&requestedAt, __ATOMIC_RELAXED), 0);
        dispatch_semaphore_signal(workerWake);
        wake_waiters();
    }
    errno = saved;
}
void skyrim_lifecycle_did_background(void) {
    if (!initialized) return;
    const int saved = errno;
    SkyrimLifecycleSnapshot s = skyrim_lifecycle_controller_snapshot(&controller);
    lifecycle_log(4, __atomic_load_n(&requestedAt, __ATOMIC_RELAXED), 0);
    /* This is an observed OS boundary, not an invented driver timeout. A late
     * flush is never reported as safe, cancelled halfway, or returned as OK. */
    if (s.phase != SKYRIM_LIFECYCLE_PAUSED)
        lifecycle_log(5, __atomic_load_n(&requestedAt, __ATOMIC_RELAXED), 0);
    errno = saved;
}
void skyrim_lifecycle_host_enter(void) {
    if (!initialized) fatal(SKYRIM_LIFECYCLE_UNINITIALIZED, "host call before initialization");
    if (hostDepth) { ++hostDepth; return; }
    const int saved = errno;
    for (;;) {
        int admitted = skyrim_lifecycle_controller_enter(&controller);
        if (admitted > 0) break;
        if (admitted < 0) fatal(SKYRIM_LIFECYCLE_DEVICE_FAILED, "GPU drain failed; cannot resume this device");
        wait_once();
    }
    hostDepth = 1;
    errno = saved;
}
void skyrim_lifecycle_host_leave(void) {
    if (!hostDepth) fatal(SKYRIM_LIFECYCLE_UNBALANCED_CALL, "unbalanced host call");
    if (--hostDepth) return;
    int wake = skyrim_lifecycle_controller_leave(&controller);
    if (wake < 0) fatal(SKYRIM_LIFECYCLE_INVALID_COUNT, "invalid host call count");
    if (wake) dispatch_semaphore_signal(workerWake);
}
void skyrim_lifecycle_main_checkpoint(void) {
    if (!initialized || !pthread_main_np() || hostDepth) return;
    const int saved = errno;
    for (;;) {
        SkyrimLifecycleSnapshot s = skyrim_lifecycle_controller_snapshot(&controller);
        if (s.phase == SKYRIM_LIFECYCLE_ACTIVE) break;
        if (s.phase == SKYRIM_LIFECYCLE_FAILED) fatal(SKYRIM_LIFECYCLE_DEVICE_FAILED, "GPU drain failed; cannot resume this device");
        wait_once();
    }
    errno = saved;
}
int skyrim_lifecycle_audio_should_pause(void) {
    if (!initialized) return 0;
    return skyrim_lifecycle_controller_snapshot(&controller).phase != SKYRIM_LIFECYCLE_ACTIVE;
}
void skyrim_lifecycle_audio_wait(void) {
    const int saved = errno;
    while (skyrim_lifecycle_audio_should_pause()) wait_once();
    errno = saved;
}
void skyrim_lifecycle_audio_event(int paused) { lifecycle_log(paused ? 7 : 8, 0, 0); }
void skyrim_lifecycle_register_device(void *device, SkyrimLifecycleDrain drain) {
    if (!device || !drain || !hostDepth) fatal(SKYRIM_LIFECYCLE_UNADMITTED_DEVICE, "device registration outside admitted host call");
    pthread_mutex_lock(&registryMutex);
    for (unsigned i = 0; i < 8; ++i)
        if (devices[i].device == device) fatal(SKYRIM_LIFECYCLE_DUPLICATE_DEVICE, "duplicate device registration");
    for (unsigned i = 0; i < 8; ++i) {
        if (!devices[i].device) {
            devices[i].device = device; devices[i].drain = drain;
            pthread_mutex_unlock(&registryMutex); return;
        }
    }
    fatal(SKYRIM_LIFECYCLE_REGISTRY_FULL, "device registry capacity exceeded");
}
void skyrim_lifecycle_unregister_device(void *device) {
    pthread_mutex_lock(&registryMutex);
    for (unsigned i = 0; i < 8; ++i) if (devices[i].device == device) {
        devices[i].device = NULL; devices[i].drain = NULL;
        pthread_mutex_unlock(&registryMutex); return;
    }
    pthread_mutex_unlock(&registryMutex);
    fatal(SKYRIM_LIFECYCLE_UNKNOWN_DEVICE, "unknown device destruction");
}
