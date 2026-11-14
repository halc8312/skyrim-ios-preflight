/* SPDX-License-Identifier: GPL-3.0-only */
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <CoreFoundation/CoreFoundation.h>
#include <os/proc.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include "device_policy.h"
#include "gpu_gate.h"
#include "performance_metrics.h"

static NSMutableArray *observers;

void skyrim_device_platform_refresh(void) {
    if (!NSThread.isMainThread) return;
    NSProcessInfo *process = NSProcessInfo.processInfo;
    SkyrimDeviceState state = {
        .active_cpus = (uint32_t)process.activeProcessorCount,
        .thermal = (uint32_t)process.thermalState,
        .low_power = process.lowPowerModeEnabled,
        .foreground = UIApplication.sharedApplication.applicationState != UIApplicationStateBackground
    };
    skyrim_device_update(&state);
}
void skyrim_device_platform_initialize(void) {
    if (!NSThread.isMainThread || observers) return;
    NSProcessInfo *process = NSProcessInfo.processInfo;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    SkyrimDeviceCaps caps = {
        .logical_cpus = (uint32_t)process.processorCount,
        .active_cpus = (uint32_t)process.activeProcessorCount,
        .display_max_fps = (uint32_t)UIScreen.mainScreen.maximumFramesPerSecond,
        .physical_memory = process.physicalMemory,
        .gpu_present = device != nil,
        .unified_memory = device.hasUnifiedMemory,
        .bc_textures = device.supportsBCTextureCompression,
        .max_buffer_length = device.maxBufferLength
    };
    const MTLGPUFamily families[] = {MTLGPUFamilyApple1, MTLGPUFamilyApple2,
        MTLGPUFamilyApple3, MTLGPUFamilyApple4, MTLGPUFamilyApple5,
        MTLGPUFamilyApple6, MTLGPUFamilyApple7, MTLGPUFamilyApple8, MTLGPUFamilyApple9};
    for (unsigned i = 0; i < sizeof families / sizeof families[0]; ++i)
        if ([device supportsFamily:families[i]]) caps.apple_gpu_family = i + 1;
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0, *)) {
        if ([device supportsFamily:MTLGPUFamilyApple10]) caps.apple_gpu_family = 10;
    }
#endif
    SkyrimDeviceState state = {.active_cpus=caps.active_cpus, .thermal=(uint32_t)process.thermalState,
        .low_power=process.lowPowerModeEnabled,
        .foreground=UIApplication.sharedApplication.applicationState != UIApplicationStateBackground};
    skyrim_device_initialize(&caps, &state);
    observers = [NSMutableArray new];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    id gpuPause = [center addObserverForName:UIApplicationWillResignActiveNotification object:nil
        queue:nil usingBlock:^(NSNotification *note) {
            (void)note; skyrim_perf_activity(0); skyrim_gpu_pause();
        }];
    id gpuResume = [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil
        queue:nil usingBlock:^(NSNotification *note) {
            (void)note; skyrim_gpu_resume(); skyrim_perf_activity(1);
        }];
    [observers addObject:gpuPause]; [observers addObject:gpuResume];
    if (UIApplication.sharedApplication.applicationState == UIApplicationStateBackground)
        skyrim_gpu_pause();
    for (NSNotificationName name in @[NSProcessInfoThermalStateDidChangeNotification,
        NSProcessInfoPowerStateDidChangeNotification, UIApplicationDidEnterBackgroundNotification,
        UIApplicationDidBecomeActiveNotification]) {
        id token = [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *note) { (void)note; skyrim_device_platform_refresh(); }];
        [observers addObject:token];
    }
    id token = [center addObserverForName:UIApplicationDidReceiveMemoryWarningNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
            (void)note; skyrim_device_memory_warning(skyrim_perf_now_ns());
        }];
    [observers addObject:token];
    fprintf(stderr, "IOS_DEVICE schema=1 cpus=%u active=%u screen_max_hz=%u physical_bytes=%llu gpu=%u apple_family=%u unified=%u bc=%u max_buffer_bytes=%llu guest_cpu_policy_unchanged=1\n",
        caps.logical_cpus, caps.active_cpus, caps.display_max_fps, (unsigned long long)caps.physical_memory,
        caps.gpu_present, caps.apple_gpu_family, caps.unified_memory, caps.bc_textures,
        (unsigned long long)caps.max_buffer_length);
}
int skyrim_device_platform_headroom(uint64_t *available) {
    /* Query at the decision point. Never persist this value as a memory limit. */
    int saved = errno;
    *available = os_proc_available_memory();
    errno = saved;
    return 1; /* In this iOS application zero is a meaningful advisory result. */
}
int skyrim_device_platform_work_role(unsigned role) {
    static _Thread_local unsigned attempted;
    static _Thread_local int last_result;
    unsigned wanted = skyrim_device_work_qos(role);
    if (wanted == attempted) return last_result;
    int saved = errno;
    qos_class_t qos = wanted == SKYRIM_QOS_BACKGROUND ? QOS_CLASS_BACKGROUND : QOS_CLASS_UTILITY;
    int result = pthread_set_qos_class_self_np(qos, 0);
    attempted = wanted;
    last_result = result;
    if (result) fprintf(stderr, "IOS_WORK_QOS role=%u target=%u error=%d\n", role, wanted, result);
    errno = saved;
    return result;
}
unsigned skyrim_device_platform_wait(uint64_t remaining) {
    int saved = errno;
    if (NSThread.isMainThread) {
        /* Input and UIKit callbacks wake the wait. No 250 Hz timer polling. */
        SInt32 result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, (double)remaining / 1e9, true);
        if (result == kCFRunLoopRunHandledSource || result == kCFRunLoopRunTimedOut) { errno = saved; return 0; }
        /* An empty/stopped run loop must not spin. Keep the old bounded fallback. */
        if (remaining > 4000000) remaining = 4000000;
    }
    struct timespec pause = {(time_t)(remaining / UINT64_C(1000000000)),
        (long)(remaining % UINT64_C(1000000000))};
    nanosleep(&pause, NULL); /* caller rechecks the deadline after an interruption */
    errno = saved;
    return 1;
}
void skyrim_device_platform_log(void) {
    SkyrimDeviceState state;
    skyrim_device_snapshot(NULL, &state);
    uint64_t available, reclaimed, reduced;
    int known = skyrim_device_platform_headroom(&available);
    skyrim_device_take_counters(&reclaimed, &reduced);
    fprintf(stderr, "IOS_DEVICE_STATE active=%u thermal=%u low_power=%u foreground=%u headroom_known=%d headroom_bytes=%llu memory_warning=%d reclaimed_pool_bytes=%llu smaller_pools=%llu\n",
        state.active_cpus, state.thermal, state.low_power, state.foreground, known,
        (unsigned long long)available, skyrim_device_memory_pressure(skyrim_perf_now_ns()),
        (unsigned long long)reclaimed, (unsigned long long)reduced);
}
