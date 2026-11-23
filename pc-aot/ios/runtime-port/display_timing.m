/* SPDX-License-Identifier: GPL-3.0-only */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display_timing.h"
#include "display_timing_core.h"
#include "performance_metrics.h"
#include "present_pacing_core.h"

/* Compile under ARC. Handlers capture scalar values only. */
static uint64_t media_ns(double seconds) {
    return seconds > 0.0 ? (uint64_t)llround(seconds * 1.0e9) : 0;
}

/* Opt-in pacing (SKYRIM_IOS_DISPLAY_SYNC=1): the frame cap becomes the drawable's minimum
 * on-screen duration. Metal's own -presentAfterMinimumDuration: does the spacing; the
 * original -present is called whenever there is no cap, and as a guard against reentry. */
static IMP original_present;
static _Thread_local int inside_present;
static void swizzled_present(id self, SEL command) {
    const uint64_t minimum = skyrim_present_target();
    if (minimum && !inside_present && [self respondsToSelector:@selector(presentAfterMinimumDuration:)]) {
        inside_present = 1;
        [(id<MTLDrawable>)self presentAfterMinimumDuration:(double)minimum * 1.0e-9];
        inside_present = 0;
        return;
    }
    ((void (*)(id, SEL))original_present)(self, command);
}
static int observe_method(Class cls, SEL selector, IMP replacement, IMP *original);
static void hook_present_once(id drawable) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const int hooked = observe_method(object_getClass(drawable), @selector(present),
                                          (IMP)swizzled_present, &original_present);
        fprintf(stderr, "IOS_DISPLAY_SYNC_HOOK schema=1 hooked=%d\n", hooked);
    });
}

@interface SkyrimRefreshTarget : NSObject
- (void)tick:(CADisplayLink *)link;
@end
@implementation SkyrimRefreshTarget
- (void)tick:(CADisplayLink *)link { (void)link; }   /* exists only to carry the refresh request */
@end
static CADisplayLink *refresh_link;
static SkyrimRefreshTarget *refresh_target;
static _Atomic unsigned requested_fps = ~0u;
static void apply_refresh(unsigned fps) {
    unsigned minimum = 0, maximum = 0, preferred = 0;
    const unsigned hz = (unsigned)UIScreen.mainScreen.maximumFramesPerSecond;
    if (!skyrim_present_refresh_request(hz, fps, &minimum, &maximum, &preferred)) {
        /* No even refresh exists for this cap (or no cap): leave the choice to the OS. */
        [refresh_link invalidate];
        refresh_link = nil;
        return;
    }
    if (!refresh_link) {
        refresh_target = [SkyrimRefreshTarget new];
        refresh_link = [CADisplayLink displayLinkWithTarget:refresh_target selector:@selector(tick:)];
        [refresh_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    refresh_link.preferredFrameRateRange = CAFrameRateRangeMake((float)minimum, (float)maximum, (float)preferred);
}
void skyrim_present_refresh_hint(unsigned fps_limit) {
    if (!skyrim_present_sync_enabled()) return;
    if (atomic_exchange_explicit(&requested_fps, fps_limit, memory_order_relaxed) == fps_limit) return;
    dispatch_async(dispatch_get_main_queue(), ^{ apply_refresh(fps_limit); });
}

static IMP original_next_drawable;
static id swizzled_next_drawable(id self, SEL command) {
    id<CAMetalDrawable> drawable = ((id (*)(id, SEL))original_next_drawable)(self, command);
    if (drawable && skyrim_present_sync_enabled()) hook_present_once(drawable);
    if (drawable) {
        const uint64_t acquired = media_ns(CACurrentMediaTime());
        /* Runs on a Metal-owned thread after the frame is shown (or dropped:
         * presentedTime == 0). Only records; never waits or signals. */
        [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            skyrim_display_presented(media_ns(shown.presentedTime), acquired,
                                     media_ns(CACurrentMediaTime()), skyrim_perf_now_ns());
        }];
    }
    return drawable;
}

static void observe_command_buffer(id<MTLCommandBuffer> buffer) {
    if (!buffer) return;
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        skyrim_display_gpu_completed(media_ns(done.GPUStartTime), media_ns(done.GPUEndTime),
                                     done.status == MTLCommandBufferStatusCompleted);
    }];
}

static IMP original_plain, original_unretained, original_descriptor;
static id swizzled_plain(id self, SEL command) {
    id<MTLCommandBuffer> buffer = ((id (*)(id, SEL))original_plain)(self, command);
    observe_command_buffer(buffer);
    return buffer;
}
static id swizzled_unretained(id self, SEL command) {
    id<MTLCommandBuffer> buffer = ((id (*)(id, SEL))original_unretained)(self, command);
    observe_command_buffer(buffer);
    return buffer;
}
static id swizzled_descriptor(id self, SEL command, MTLCommandBufferDescriptor *descriptor) {
    id<MTLCommandBuffer> buffer =
        ((id (*)(id, SEL, MTLCommandBufferDescriptor *))original_descriptor)(self, command, descriptor);
    observe_command_buffer(buffer);
    return buffer;
}

/* Replace on `cls` itself. If the method is inherited, class_replaceMethod adds
 * an override on cls (calling the inherited IMP we saved) and leaves the parent
 * class untouched. Returns 0 and leaves `*original` NULL if the selector is
 * absent, so the status line reports what really happened. */
static int observe_method(Class cls, SEL selector, IMP replacement, IMP *original) {
    Method method = class_getInstanceMethod(cls, selector);
    if (!method || *original) return 0;
    IMP previous = method_getImplementation(method);
    if (!previous) return 0;
    *original = previous;
    class_replaceMethod(cls, selector, replacement, method_getTypeEncoding(method));
    return 1;
}

SkyrimDisplayTimingStatus skyrim_display_timing_install(void) {
    static int installed;
    SkyrimDisplayTimingStatus status = {0, 0, 0};
    const char *setting = getenv("SKYRIM_IOS_DISPLAY_TIMING");
    if (setting && !strcmp(setting, "0")) {
        status.disabled = 1;
    } else if (!installed) {
        installed = 1;
        status.presented_installed = (unsigned)observe_method(
            [CAMetalLayer class], @selector(nextDrawable), (IMP)swizzled_next_drawable,
            &original_next_drawable);
        @autoreleasepool {
            /* The concrete queue class is private and device specific, so find it
             * from a real queue instead of naming it. MoltenVK creates its queue
             * from the same device and so receives the same class. */
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            id<MTLCommandQueue> probe = [device newCommandQueue];
            if (probe) {
                Class cls = object_getClass(probe);
                status.gpu_methods += (unsigned)observe_method(
                    cls, @selector(commandBuffer), (IMP)swizzled_plain, &original_plain);
                status.gpu_methods += (unsigned)observe_method(
                    cls, @selector(commandBufferWithUnretainedReferences),
                    (IMP)swizzled_unretained, &original_unretained);
                status.gpu_methods += (unsigned)observe_method(
                    cls, @selector(commandBufferWithDescriptor:),
                    (IMP)swizzled_descriptor, &original_descriptor);
            }
        }
    } else {
        status.presented_installed = original_next_drawable != NULL;
        status.gpu_methods = (original_plain != NULL) + (original_unretained != NULL) +
                             (original_descriptor != NULL);
    }
    skyrim_display_reset();
    fprintf(stderr, "IOS_DISPLAY_SYNC schema=1 enabled=%d\n", skyrim_present_sync_enabled());
    fprintf(stderr, "IOS_DISPLAY_TIMING schema=1 disabled=%u presented=%u gpu_methods=%u\n",
            status.disabled, status.presented_installed, status.gpu_methods);
    return status;
}
