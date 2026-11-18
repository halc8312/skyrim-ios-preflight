/* Public Metal buffers only. macOS validation is not an iPhone lifecycle test. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu_gate.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
extern void skyrim_gpu_commit_metal(void *buffer);
extern void skyrim_gpu_submission_enter(void);
extern void skyrim_gpu_submission_leave(void);
static atomic_int attempted, returned;
static void *worker(void *raw) {
    @autoreleasepool {
        atomic_store(&attempted,1);
        skyrim_gpu_submission_enter();
        id<MTLCommandBuffer> cb=(__bridge id<MTLCommandBuffer>)raw;
        [cb enqueue];
        skyrim_gpu_commit_metal(raw);
        skyrim_gpu_submission_leave();
        atomic_store(&returned,1);
    }
    return NULL;
}
int main(void) { @autoreleasepool {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();assert(device);
    id<MTLCommandQueue> first=[device newCommandQueue],second=[device newCommandQueue];assert(first && second);
    id<MTLBuffer> a=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> b=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];assert(a && b);
    id<MTLCommandBuffer> ca=[first commandBuffer], cb=[second commandBuffer];
    id<MTLBlitCommandEncoder> enc=[ca blitCommandEncoder];[enc fillBuffer:a range:NSMakeRange(0,256) value:0x39];[enc endEncoding];
    enc=[cb blitCommandEncoder];[enc fillBuffer:b range:NSMakeRange(0,256) value:0x72];[enc endEncoding];
    skyrim_gpu_commit_metal((__bridge void *)ca);
    skyrim_gpu_pause();assert(ca.status>=MTLCommandBufferStatusScheduled);
    pthread_t thread;assert(!pthread_create(&thread,NULL,worker,(__bridge void *)cb));
    for(unsigned i=0;!atomic_load(&attempted) && i<1000;++i) usleep(1000);
    assert(atomic_load(&attempted));usleep(20000);assert(!atomic_load(&returned));
    assert(cb.status==MTLCommandBufferStatusNotEnqueued);
    skyrim_gpu_resume();assert(!pthread_join(thread,NULL));assert(atomic_load(&returned));
    skyrim_gpu_pause();assert(cb.status>=MTLCommandBufferStatusScheduled);
    /* Completion is requested only by this pixel-validation fixture, never by
     * the lifecycle implementation. Preserve both command-buffer error states. */
    [ca waitUntilCompleted];[cb waitUntilCompleted];assert(!ca.error && !cb.error);
    for(unsigned i=0;i<256;++i) {assert(((uint8_t*)a.contents)[i]==0x39);assert(((uint8_t*)b.contents)[i]==0x72);}
    skyrim_gpu_resume();
    /* Scheduling a buffer containing a future event wait must not require the
     * producer to enter the closed gate. If a driver does require it, the
     * external process deadline fails this gate; do not claim UIKit safety. */
    for(unsigned shared=0;shared<2;++shared) {
        id<MTLEvent> event=shared?[device newSharedEvent]:[device newEvent];assert(event);
        id<MTLCommandBuffer> consumer=[first commandBuffer],producer=[second commandBuffer];
        [consumer encodeWaitForEvent:event value:1];
        id<MTLBlitCommandEncoder> fill=[consumer blitCommandEncoder];
        [fill fillBuffer:a range:NSMakeRange(0,256) value:(uint8_t)(0x59+shared)];[fill endEncoding];
        [producer encodeSignalEvent:event value:1];
        skyrim_gpu_commit_metal((__bridge void *)consumer);
        skyrim_gpu_pause();assert(consumer.status>=MTLCommandBufferStatusScheduled);
        atomic_store(&attempted,0);atomic_store(&returned,0);
        assert(!pthread_create(&thread,NULL,worker,(__bridge void *)producer));
        for(unsigned i=0;!atomic_load(&attempted) && i<1000;++i)usleep(1000);
        assert(atomic_load(&attempted) && !atomic_load(&returned));
        skyrim_gpu_resume();assert(!pthread_join(thread,NULL));
        skyrim_gpu_pause();[consumer waitUntilCompleted];[producer waitUntilCompleted];
        assert(!consumer.error && !producer.error);
        for(unsigned i=0;i<256;++i)assert(((uint8_t*)a.contents)[i]==0x59+shared);
        skyrim_gpu_resume();
    }
    /* Reserve the earliest queue position, fill every tracking slot with work
     * behind it, then commit that earliest buffer through the full-slot path. */
    NSMutableArray<id<MTLCommandBuffer>> *behind=[NSMutableArray new];
    id<MTLCommandQueue> pressure=[device newCommandQueueWithMaxCommandBufferCount:SKYRIM_GPU_GATE_SLOTS+2];assert(pressure);
    skyrim_gpu_submission_enter();
    id<MTLCommandBuffer> earliest=[pressure commandBuffer];[earliest enqueue];
    for(unsigned i=0;i<SKYRIM_GPU_GATE_SLOTS;++i) {
        id<MTLCommandBuffer> next=[pressure commandBuffer];
        [behind addObject:next];skyrim_gpu_commit_metal((__bridge void *)next);
    }
    skyrim_gpu_commit_metal((__bridge void *)earliest);skyrim_gpu_submission_leave();
    skyrim_gpu_pause();[earliest waitUntilCompleted];assert(!earliest.error);
    for(id<MTLCommandBuffer> next in behind) {
        assert(next.status>=MTLCommandBufferStatusScheduled);[next waitUntilCompleted];assert(!next.error);
    }
    skyrim_gpu_resume();
    puts("GPU_METAL_GATE two_queues_exact_bytes=PASS scheduling=PASS event_dependencies=PASS early_enqueue_full_capacity=PASS public_only=1 iphone_verified=0");
} }
