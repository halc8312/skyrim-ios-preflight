#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <pthread.h>
#include "MetalConstantProbe.h"
#include "MetalConstantValidation.h"
#if !__has_feature(objc_arc)
#error "MetalConstantProbe.mm requires ARC"
#endif

namespace {
using Clock = std::chrono::steady_clock;
static uint64_t elapsed(Clock::time_point at) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-at).count());
}
struct Context {
    pthread_t owner = pthread_self();
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline;
    id<MTLBuffer> records, values, blocks, readback;
    size_t prepared = 0;
};
static bool same_thread(Context *c) { return c && pthread_equal(c->owner, pthread_self()); }
}
extern "C" void *OHMetalConstantCreateV1(void) {
    @autoreleasepool {
        auto c = new(std::nothrow) Context;
        if (!c) return nullptr;
        c->device = MTLCreateSystemDefaultDevice();
        c->queue = [c->device newCommandQueue];
        // Xcode compiles the authored .metal file offline for the exact SDK target.
        id<MTLLibrary> library = [c->device newDefaultLibrary];
        id<MTLFunction> fn = [library newFunctionWithName:@"owned_constant_replay_v1"];
        NSError *error = nil;
        if (fn) c->pipeline = [c->device newComputePipelineStateWithFunction:fn error:&error];
        c->records = [c->device newBufferWithLength:(ohcp::bin_header_words+(ohcp::record_words+1)*ohcp::max_records)*4 options:MTLResourceStorageModeShared];
        c->values = [c->device newBufferWithLength:ohcp::max_values*16 options:MTLResourceStorageModeShared];
        c->blocks = [c->device newBufferWithLength:ohcp::block_bytes options:MTLResourceStorageModeShared];
        c->readback = [c->device newBufferWithLength:ohcp::block_bytes options:MTLResourceStorageModeShared];
        if (!c->queue || !c->pipeline || c->pipeline.maxTotalThreadsPerThreadgroup < 256
            || !c->records || !c->values || !c->blocks || !c->readback) {
            fprintf(stderr,"PUBLIC_METAL_CONSTANT_CREATE library=%d function=%d pipeline=%d error=%s\n",
                library != nil, fn != nil, c->pipeline != nil, error.localizedDescription.UTF8String ?: "none");
            delete c; return nullptr;
        }
        return c;
    }
}
extern "C" int32_t OHMetalConstantDestroyV1(void *context) {
    auto c = static_cast<Context *>(context);
    if (!same_thread(c)) return -1;
    delete c; return 0;
}
extern "C" int32_t OHMetalConstantLimitsV1(uint64_t *out, size_t count) {
    if (!out || count != 7 || reinterpret_cast<uintptr_t>(out) % alignof(uint64_t)) return -1;
    const uint64_t limits[] = {ohcp::stages, ohcp::lanes, ohcp::record_words, ohcp::bin_header_words,
        ohcp::max_records, ohcp::max_values, ohcp::block_bytes};
    memcpy(out, limits, sizeof(limits)); return 0;
}
extern "C" int32_t OHMetalConstantPrepareV1(void *context, size_t bytes) {
    auto c = static_cast<Context *>(context);
    if (!same_thread(c) || !bytes || bytes > ohcp::block_bytes || bytes % 16) return -1;
    // Every run below waits for its consumer. No in-flight resource may be overwritten.
    memset(c->blocks.contents, 0xa5, bytes); memset(c->readback.contents, 0x5a, bytes);
    c->prepared = bytes; return 0;
}
extern "C" int32_t OHMetalConstantRunV1(void *context, const uint32_t *records, size_t words,
    const uint8_t *values, size_t value_bytes, uint8_t *output, size_t output_bytes,
    OHConstantTimesV1 *out_times) {
    auto c = static_cast<Context *>(context);
    if (!same_thread(c) || !records || (!values && value_bytes) || !output || !out_times
        || reinterpret_cast<uintptr_t>(records) % alignof(uint32_t)
        || reinterpret_cast<uintptr_t>(out_times) % alignof(OHConstantTimesV1)
        || c->prepared != output_bytes) return -1;
    // Allocation failure in the bounded validator must not unwind through Rust's C ABI.
    try { if (!ohcp::validate(records, words, value_bytes, output_bytes)) return -1; }
    catch (...) { return -4; }
    @autoreleasepool {
        OHConstantTimesV1 times{};
        auto start = Clock::now(), at = start;
        memcpy(c->records.contents, records, words*4);
        if (value_bytes) memcpy(c->values.contents, values, value_bytes);
        times.upload_ns = elapsed(at); at = Clock::now();
        id<MTLCommandBuffer> command = [c->queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        if (!command || !compute) return -2;
        [compute setComputePipelineState:c->pipeline];
        [compute setBuffer:c->records offset:0 atIndex:0];
        [compute setBuffer:c->values offset:0 atIndex:1];
        [compute setBuffer:c->blocks offset:0 atIndex:2];
        [compute dispatchThreadgroups:MTLSizeMake(ohcp::stages,1,1) threadsPerThreadgroup:MTLSizeMake(ohcp::lanes,1,1)];
        // Metal v1 phase semantics: finish includes endEncoding AND blit recording.
        // This differs from wgpu's deferred finish; only complete-path times are compared.
        times.encode_ns = elapsed(at); at = Clock::now();
        [compute endEncoding];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!blit) return -2;
        [blit copyFromBuffer:c->blocks sourceOffset:0 toBuffer:c->readback destinationOffset:0 size:output_bytes];
        [blit endEncoding];
        times.finish_ns = elapsed(at); at = Clock::now();
        [command commit]; times.submit_ns = elapsed(at); at = Clock::now();
        [command waitUntilCompleted]; times.completion_ns = elapsed(at);
        if (command.status != MTLCommandBufferStatusCompleted || command.error) return -3;
        at = Clock::now(); memcpy(output, c->readback.contents, output_bytes);
        times.readback_ns = elapsed(at); times.total_ns = elapsed(start);
        *out_times = times; return 0;
    }
}
