/* SPDX-License-Identifier: GPL-3.0-only */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreFoundation/CoreFoundation.h>
#include "gpu_gate.h"
#include <pthread.h>
#include <stdio.h>

/* Compile under ARC. The C gate owns an explicit CF retain for each slot. */
static SkyrimGpuGate gate = SKYRIM_GPU_GATE_INIT;
static void hold(void *p) { CFRetain((CFTypeRef)p); }
static void drop(void *p) { CFRelease((CFTypeRef)p); }
static int scheduled(void *p) {
    return [(__bridge id<MTLCommandBuffer>)p status] >= MTLCommandBufferStatusScheduled;
}
static void commit(void *p) { [(__bridge id<MTLCommandBuffer>)p commit]; }
static void wait_scheduled(void *p) { [(__bridge id<MTLCommandBuffer>)p waitUntilScheduled]; }
static const SkyrimGpuGateOps ops = {hold, drop, scheduled, commit, wait_scheduled};
static void pump(void) {
    @autoreleasepool {
        /* Runs UIKit activity notifications without advancing the guest. */
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
}
void skyrim_gpu_commit_metal(void *buffer) {
    skyrim_gpu_gate_commit(&gate, &ops, buffer, pthread_main_np() ? pump : NULL);
}
void skyrim_gpu_submission_enter(void) {
    skyrim_gpu_gate_enter(&gate, pthread_main_np() ? pump : NULL);
}
void skyrim_gpu_submission_leave(void) {
    skyrim_gpu_gate_leave(&gate);
}
void skyrim_gpu_pause(void) {
    NSCAssert(NSThread.isMainThread, @"GPU pause must run on UIKit main thread");
    NSTimeInterval start = NSProcessInfo.processInfo.systemUptime;
    skyrim_gpu_gate_pause(&gate, &ops);
    fprintf(stderr, "IOS_GPU_LIFECYCLE event=paused host_ms=%.3f completion_wait=0\n",
            (NSProcessInfo.processInfo.systemUptime - start) * 1000.0);
}
void skyrim_gpu_resume(void) {
    NSCAssert(NSThread.isMainThread, @"GPU resume must run on UIKit main thread");
    skyrim_gpu_gate_resume(&gate);
    fputs("IOS_GPU_LIFECYCLE event=resumed\n", stderr);
}
void skyrim_gpu_wait_active(void) {
    skyrim_gpu_gate_wait(&gate, pthread_main_np() ? pump : NULL);
}
