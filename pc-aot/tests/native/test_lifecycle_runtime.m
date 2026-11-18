/* Real CFRunLoop and public Metal buffers. macOS is not iPhone suspension. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include "lifecycle_runtime.h"

@interface LifecycleFixtureDevice : NSObject
@property(nonatomic, strong) id<MTLCommandBuffer> consumer;
@property(nonatomic, strong) id<MTLCommandBuffer> producer;
@end
@implementation LifecycleFixtureDevice
@end

static atomic_int drain_started, release_drain, ui_handled, drain_completed;
static atomic_int unregister_started, unregister_done;
static void *retiring_device;
static void nap(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static void wait_flag(atomic_int *flag) {
    for (unsigned i = 0; i < 5000 && !atomic_load(flag); ++i) nap();
    assert(atomic_load(flag));
}
static int32_t drain(void *raw, unsigned phase) {
    assert(!pthread_main_np());
    LifecycleFixtureDevice *d = (__bridge LifecycleFixtureDevice *)raw;
    if (phase == 0) {
        atomic_store(&drain_started, 1);
        wait_flag(&release_drain);
        // Commit the missing producer before any wait. Nothing in the new
        // controller prevents native submissions by previously admitted work.
        [d.producer commit];
    } else if (phase == 2) {
        [d.producer waitUntilCompleted];
        [d.consumer waitUntilCompleted];
        assert(!d.consumer.error && !d.producer.error);
        atomic_store(&drain_completed, 1);
    }
    return 0;
}
static void *resume_notification(void *unused) {
    (void)unused;
    @autoreleasepool {
        wait_flag(&drain_started);
        if (retiring_device) {
            wait_flag(&unregister_started);
            assert(!atomic_load(&unregister_done));
        }
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
            assert(pthread_main_np());
            // Background callback returns without waiting for a stuck drain.
            // This deliberately logs deadline_missed, never a false success.
            skyrim_lifecycle_did_background();
            skyrim_lifecycle_request(1);
            assert(skyrim_lifecycle_audio_should_pause());
            atomic_store(&ui_handled, 1);
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        wait_flag(&ui_handled);
        assert(!atomic_load(&drain_completed));
        atomic_store(&release_drain, 1);
    }
    return NULL;
}
static void *retire_registration(void *unused) {
    (void)unused;
    wait_flag(&drain_started);
    atomic_store(&unregister_started, 1);
    skyrim_lifecycle_unregister_device(retiring_device);
    assert(atomic_load(&drain_completed));
    atomic_store(&unregister_done, 1);
    return NULL;
}
static uint64_t now_ns(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}
int main(void) { @autoreleasepool {
    skyrim_lifecycle_initialize();
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice(); assert(gpu);
    id<MTLCommandQueue> first = [gpu newCommandQueue], second = [gpu newCommandQueue];
    id<MTLBuffer> data = [gpu newBufferWithLength:256 options:MTLResourceStorageModeShared];
    assert(first && second && data);
    const unsigned bench_calls = 100000;
    const uint64_t bench_begin = now_ns();
    for (unsigned i = 0; i < bench_calls; ++i) {
        skyrim_lifecycle_host_enter(); skyrim_lifecycle_host_leave();
    }
    const uint64_t bench_elapsed = now_ns() - bench_begin;
    printf("LIFECYCLE_BENCH calls=%u elapsed_ns=%llu ns_per_call=%llu iphone_verified=0\n",
        bench_calls, (unsigned long long)bench_elapsed,
        (unsigned long long)(bench_elapsed / bench_calls));
    for (unsigned shared = 0; shared < 2; ++shared) {
        atomic_store(&drain_started, 0); atomic_store(&release_drain, 0);
        atomic_store(&ui_handled, 0); atomic_store(&drain_completed, 0);
        id<MTLEvent> event = shared ? [gpu newSharedEvent] : [gpu newEvent]; assert(event);
        // The registry holds a borrowed pointer until both worker threads join.
        __attribute__((objc_precise_lifetime)) LifecycleFixtureDevice *d =
            [LifecycleFixtureDevice new];
        d.consumer = [first commandBuffer]; d.producer = [second commandBuffer];
        [d.consumer encodeWaitForEvent:event value:1];
        id<MTLBlitCommandEncoder> encoder = [d.consumer blitCommandEncoder];
        [encoder fillBuffer:data range:NSMakeRange(0, 256) value:(uint8_t)(0x51 + shared)];
        [encoder endEncoding];
        [d.producer encodeSignalEvent:event value:1];
        skyrim_lifecycle_host_enter();
        skyrim_lifecycle_register_device((__bridge void *)d, drain);
        [d.consumer commit];
        skyrim_lifecycle_host_leave();
        skyrim_lifecycle_request(0);
        pthread_t notifier, retire;
        if (shared) {
            retiring_device = (__bridge void *)d;
            assert(!pthread_create(&retire, NULL, retire_registration, NULL));
        }
        assert(!pthread_create(&notifier, NULL, resume_notification, NULL));
        skyrim_lifecycle_main_checkpoint();
        assert(atomic_load(&ui_handled) && atomic_load(&drain_completed));
        assert(!pthread_join(notifier, NULL));
        for (unsigned i = 0; i < 256; ++i)
            assert(((uint8_t *)data.contents)[i] == (uint8_t)(0x51 + shared));
        if (shared) {
            assert(!pthread_join(retire, NULL));
            assert(atomic_load(&unregister_started) && atomic_load(&unregister_done));
        } else {
            skyrim_lifecycle_host_enter();
            skyrim_lifecycle_unregister_device((__bridge void *)d);
            skyrim_lifecycle_host_leave();
        }
    }
    puts("lifecycle main_runloop_live=PASS event_dependency_bytes=PASS rapid_resume=PASS registry_lifetime=PASS iphone_verified=0");
} }
