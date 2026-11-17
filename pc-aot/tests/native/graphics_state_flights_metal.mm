// Real Mac Metal PSOs/readback behind actual DXVK coordination methods.
// Authored shaders only: not full DXVK/MoltenVK, iPhone gameplay or performance.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
// Foundation's Objective-C BOOL and DXVK Native's Win32 BOOL have different
// underlying types. Isolate the compatibility typedef in this mixed-language
// fixture; all DXVK declarations still see their original Win32 integer ABI.
#define BOOL DxvkNativeBOOL
#include "dxvk/dxvk_bind_mask.h"
#include "dxvk/dxvk_graphics_state.h"
#include "util/sync/sync_spinlock.h"
#undef BOOL
#include "performance_metrics.h"
#include "prefetch_gate.h"
#include "graphics_flights.h"

using namespace std::chrono_literals;
using State = dxvk::DxvkGraphicsPipelineStateInfo;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void metal_require(bool condition, NSError* error, const char* fallback) {
    if (!condition) throw std::runtime_error(error.localizedDescription.UTF8String ?: fallback);
}
namespace dxvk {
class DxvkRenderPass {
public:
    explicit DxvkRenderPass(unsigned value = 0) : tag(value) {}
    unsigned format() const { return 73; }
    const unsigned tag;
};
}
using Pass = dxvk::DxvkRenderPass;
static State state(unsigned value) { State result; result.sc.specConstants[0] = value; return result; }
static unsigned token(const State& input, const Pass* pass) {
    return input.sc.specConstants[0]
        + input.ilBindings[dxvk::DxvkLimits::MaxNumVertexBindings - 1].stride() + pass->tag * 16;
}

class Signal {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count = 0;
public:
    void notify() { std::lock_guard<std::mutex> lock(mutex); ++count; changed.notify_all(); }
    bool wait(unsigned target = 1) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 30s, [&] { return count >= target; });
    }
};
struct DriverGate {
    Signal entered, released;
    void hold() { entered.notify(); require(released.wait(), "Authored driver gate deadline"); }
    void open() { released.notify(); }
};
template<typename T> static bool finished(std::future<T>& future) {
    return future.wait_for(30s) == std::future_status::ready;
}

thread_local bool fail_publication = false;
template<typename T> struct PublicationAllocator {
    using value_type = T;
    PublicationAllocator() = default;
    template<typename U> PublicationAllocator(const PublicationAllocator<U>&) noexcept {}
    T* allocate(size_t count) {
        if (fail_publication) { fail_publication = false; throw std::bad_alloc(); }
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, size_t count) noexcept { std::allocator<T>{}.deallocate(pointer, count); }
};
template<typename T, typename U> bool operator==(const PublicationAllocator<T>&, const PublicationAllocator<U>&) { return true; }
template<typename T, typename U> bool operator!=(const PublicationAllocator<T>&, const PublicationAllocator<U>&) { return false; }
struct InjectedDriverFailure : std::exception {};
struct Totals {
    std::atomic<unsigned> created{0}, released{0}, live{0}, rejected{0}, exceptions{0}, publication_failures{0};
};

struct MetalContext {
    id<MTLDevice> device;
    id<MTLLibrary> library;
    id<MTLFunction> vertex;
    id<MTLCommandQueue> queue;
    explicit MetalContext(id<MTLDevice> selected) : device(selected) {
        NSString* source = [NSString stringWithUTF8String:R"MSL(
#include <metal_stdlib>
using namespace metal;
constant uint color_code [[function_constant(0)]];
vertex float4 flight_vertex(uint index [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(positions[index], 0, 1);
}
fragment float4 flight_fragment() {
    return float4(float(color_code), float(color_code + 1u), float(color_code + 2u), 1.0f);
}
// Valid library function, deliberately incompatible with flight_vertex's output.
// The real render-PSO linker must reject this missing interpolant with NSError.
struct MissingInput { float4 absent [[user(absent)]]; };
fragment float4 flight_bad_fragment(MissingInput input [[stage_in]]) { return input.absent; }
)MSL"];
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        NSError* error = nil;
        library = [device newLibraryWithSource:source options:options error:&error];
        metal_require(library != nil, error, "Authored Metal library unavailable");
        vertex = [library newFunctionWithName:@"flight_vertex"];
        queue = [device newCommandQueue];
        require(vertex != nil && queue != nil, "Authored vertex or command queue unavailable");
    }
    id<MTLRenderPipelineState> create(const State& input, const Pass* pass, bool reject) {
        NSError* error = nil;
        id<MTLFunction> fragment;
        if (reject) {
            fragment = [library newFunctionWithName:@"flight_bad_fragment"];
        } else {
            MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
            uint32_t value = token(input, pass);
            [constants setConstantValue:&value type:MTLDataTypeUInt atIndex:0];
            fragment = [library newFunctionWithName:@"flight_fragment" constantValues:constants error:&error];
        }
        metal_require(fragment != nil, error, "Authored Metal fragment specialization failed");
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        error = nil;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (reject) {
            require(pipeline == nil && error != nil, "Real Metal linker did not reject the incompatible fixture");
            std::fprintf(stderr, "Expected real Metal PSO rejection: %s\n", error.localizedDescription.UTF8String);
        } else {
            metal_require(pipeline != nil, error, "Real Metal render PSO creation failed");
        }
        return pipeline;
    }
};

struct Driver {
    struct Record { State state; const Pass* pass; unsigned calls = 0, waits = 0; VkPipeline latest = VK_NULL_HANDLE; };
    struct Block { State state; const Pass* pass; DriverGate* gate; };
    MetalContext& metal;
    Totals& totals;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Record> records;
    std::vector<Block> blocks;
    std::map<unsigned, unsigned> throws, allocation_failures, validations;
    NSMutableDictionary<NSNumber*, id<MTLRenderPipelineState>>* retained;
    uintptr_t next_handle = 1;
    DriverGate* destroy_gate = nullptr;
    bool destroyed_once = false;
    Driver(MetalContext& context, Totals& counts) : metal(context), totals(counts), retained([NSMutableDictionary new]) {}
    Record& record(const State& input, const Pass* pass) {
        for (auto& value : records) if (value.pass == pass && value.state == input) return value;
        records.push_back({input, pass}); return records.back();
    }
    void block(const State& input, const Pass* pass, DriverGate& gate) {
        std::lock_guard<std::mutex> lock(mutex); blocks.push_back({input, pass, &gate});
    }
    bool validate(const State& input) {
        std::lock_guard<std::mutex> lock(mutex);
        ++validations[input.sc.specConstants[0]];
        return input.sc.specConstants[0] != 9999;
    }
    VkPipeline create(const State& input, const Pass* pass) {
        @autoreleasepool {
            DriverGate* gate = nullptr;
            bool throw_now, allocation_failure;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++record(input, pass).calls;
                const unsigned value = input.sc.specConstants[0];
                throw_now = throws[value] != 0; if (throw_now) --throws[value];
                allocation_failure = allocation_failures[value] != 0; if (allocation_failure) --allocation_failures[value];
                for (const auto& block : blocks) if (block.pass == pass && block.state == input) gate = block.gate;
            }
            // A controlled boundary establishes concurrent admission. No test
            // infers internal Metal compiler overlap or speed from this barrier.
            if (gate) gate->hold();
            const bool reject = input.sc.specConstants[0] == 13;
            id<MTLRenderPipelineState> pipeline = metal.create(input, pass, reject);
            if (reject) { ++totals.rejected; return VK_NULL_HANDLE; }
            VkPipeline handle;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const uintptr_t value = next_handle++;
                handle = reinterpret_cast<VkPipeline>(value);
                retained[@(value)] = pipeline;
                record(input, pass).latest = handle;
                ++totals.created; ++totals.live;
            }
            // This exception is specifically authored and typed; unexpected Metal
            // errors are never accepted as the expected owner exception.
            if (throw_now) { destroy(handle); ++totals.exceptions; throw InjectedDriverFailure(); }
            if (allocation_failure) { ++totals.publication_failures; fail_publication = true; }
            return handle;
        }
    }
    void destroy(VkPipeline handle) {
        if (handle == VK_NULL_HANDLE) return;
        DriverGate* gate = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            NSNumber* key = @(reinterpret_cast<uintptr_t>(handle));
            require(retained[key] != nil, "Double or unknown fixture PSO release");
            [retained removeObjectForKey:key];
            ++totals.released; --totals.live;
            if (destroy_gate && !destroyed_once) { destroyed_once = true; gate = destroy_gate; }
        }
        if (gate) gate->hold(); // Fixture-owned retain already dropped before wait.
    }
    void write(const State&, unsigned format) { require(format == 73, "Unexpected render-pass format"); }
    void note_wait(const State& input, const Pass* pass) {
        std::lock_guard<std::mutex> lock(mutex); ++record(input, pass).waits; changed.notify_all();
    }
    bool wait_for_waiters(const State& input, const Pass* pass, unsigned target) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 30s, [&] { return record(input, pass).waits >= target; });
    }
    unsigned count(const State& input, const Pass* pass) {
        std::lock_guard<std::mutex> lock(mutex); return record(input, pass).calls;
    }
    unsigned live() { std::lock_guard<std::mutex> lock(mutex); return unsigned(retained.count); }
    VkPipeline latest(const State& input, const Pass* pass) {
        std::lock_guard<std::mutex> lock(mutex); return record(input, pass).latest;
    }
    id<MTLRenderPipelineState> pipeline(VkPipeline handle) {
        std::lock_guard<std::mutex> lock(mutex);
        id<MTLRenderPipelineState> value = retained[@(reinterpret_cast<uintptr_t>(handle))];
        require(value != nil, "Rendering an absent or released PSO");
        return value;
    }
};

// ACTUAL_DXVK_METHODS

static void render(NSMutableArray* results, NSString* name, MetalContext& metal,
                   Driver& driver, VkPipeline handle, unsigned expected_token) {
    @autoreleasepool {
        id<MTLRenderPipelineState> pipeline = driver.pipeline(handle);
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> target = [metal.device newTextureWithDescriptor:descriptor];
        id<MTLBuffer> readback = [metal.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        require(target != nil && readback != nil, "Metal target/readback allocation failed");
        std::memset(readback.contents, 0xa7, 256);
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(-123, -124, -125, -126);
        id<MTLCommandBuffer> command = [metal.queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        require(command != nil && encoder != nil, "Metal render encoder unavailable");
        [encoder setRenderPipelineState:pipeline];
        [encoder setViewport:(MTLViewport){0, 0, 1, 1, 0, 1}];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(blit != nil, "Metal readback encoder unavailable");
        [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(1, 1, 1) toBuffer:readback destinationOffset:0
            destinationBytesPerRow:256 destinationBytesPerImage:256];
        [blit endEncoding];
        [command commit]; [command waitUntilCompleted]; // Python process deadline remains mandatory.
        metal_require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
                      command.error, "Metal GPU command did not complete successfully");
        float actual[4]; uint32_t bits[4];
        std::memcpy(actual, readback.contents, sizeof actual);
        std::memcpy(bits, readback.contents, sizeof bits);
        const float expected[4] = {float(expected_token), float(expected_token + 1), float(expected_token + 2), 1.0f};
        for (unsigned lane = 0; lane < 4; ++lane)
            require(actual[lane] == expected[lane], "Authored Metal readback mismatch");
        [results addObject:@{@"case":name, @"matches":@YES,
            @"bits":@[@(bits[0]), @(bits[1]), @(bits[2]), @(bits[3])]}];
    }
}

static bool coordination_cases(MetalContext& metal, Totals& totals, NSMutableArray* results) {
    Driver driver(metal, totals), other_driver(metal, totals);
    Manager manager; Pass pass, same_format_pass(1);
    DriverGate blocked;
    bool progress;
    {
        TestPipeline pipeline(driver, manager), other(other_driver, manager);
        const auto warm = state(1), cold = state(2), distinct = state(3);
        auto packed = cold;
        packed.ilBindings[dxvk::DxvkLimits::MaxNumVertexBindings - 1] =
            dxvk::DxvkIlBinding(1, 8, VK_VERTEX_INPUT_RATE_INSTANCE, 2);
        const auto warm_handle = pipeline.get(warm, &pass);
        driver.block(cold, &pass, blocked);
        auto owner = std::async(std::launch::async, [&] { @autoreleasepool { pipeline.build(cold, &pass); } });
        const bool entered = blocked.entered.wait();
        auto ready = std::async(std::launch::async, [&] { @autoreleasepool { return pipeline.get(warm, &pass); } });
        const bool ready_done = finished(ready);
        auto independent = std::async(std::launch::async, [&] { @autoreleasepool { return other.get(cold, &pass); } });
        const bool independent_done = finished(independent);
        auto different = std::async(std::launch::async, [&] { @autoreleasepool { return pipeline.get(distinct, &pass); } });
        auto packed_variant = std::async(std::launch::async, [&] { @autoreleasepool { return pipeline.get(packed, &pass); } });
        auto pass_variant = std::async(std::launch::async, [&] { @autoreleasepool { return pipeline.get(cold, &same_format_pass); } });
        bool ordered;
#if PROTOTYPE
        progress = finished(different) && finished(packed_variant) && finished(pass_variant);
        ordered = progress;
#else
        ordered = driver.wait_for_waiters(distinct, &pass, 1)
            && driver.wait_for_waiters(packed, &pass, 1)
            && driver.wait_for_waiters(cold, &same_format_pass, 1);
        progress = driver.count(distinct, &pass) != 0 || driver.count(packed, &pass) != 0
            || driver.count(cold, &same_format_pass) != 0;
#endif
        auto duplicate = std::async(std::launch::async, [&] { @autoreleasepool { return pipeline.get(cold, &pass); } });
        auto prefetch = std::async(std::launch::async, [&] { @autoreleasepool { pipeline.build(cold, &pass); } });
        const bool waited = driver.wait_for_waiters(cold, &pass, 2);
        blocked.open(); // Release controlled work before checking either A/B outcome.
        owner.get(); prefetch.get();
        const auto cold_handle = duplicate.get(), ready_handle = ready.get(), independent_handle = independent.get();
        const auto different_handle = different.get(), packed_handle = packed_variant.get(), pass_handle = pass_variant.get();
        require(entered && ready_done && independent_done && ordered && waited && progress == bool(PROTOTYPE),
                "Actual DXVK graphics coordination ordering failed");
        require(ready_handle == warm_handle && cold_handle == driver.latest(cold, &pass), "Cached handle changed");
        require(driver.count(cold, &pass) == 1 && other_driver.count(cold, &pass) == 1, "Same identity compiled twice");
        require(driver.count(packed, &pass) == 1 && driver.count(cold, &same_format_pass) == 1
                && pass.format() == same_format_pass.format() && packed_handle != cold_handle && pass_handle != cold_handle,
                "Full state/render-pass pointer identity was not preserved");
        render(results, @"warm_ready", metal, driver, ready_handle, 1);
        render(results, @"cold_owner", metal, driver, cold_handle, 2);
        render(results, @"different_state", metal, driver, different_handle, 3);
        render(results, @"packed_variant", metal, driver, packed_handle, 10);
        render(results, @"different_pass", metal, driver, pass_handle, 18);
        render(results, @"independent_owner", metal, other_driver, independent_handle, 2);
        const size_t before_invalid = pipeline.size();
        require(pipeline.get(state(9999), &pass) == VK_NULL_HANDLE, "Invalid state unexpectedly created a PSO");
        pipeline.build(state(9999), &pass);
        require(pipeline.size() == before_invalid && driver.count(state(9999), &pass) == 0,
                "Invalid state was cached or sent to Metal");
        require(pipeline.get(state(13), &pass) == VK_NULL_HANDLE, "Expected real linker failure was not null");
        require(pipeline.get(state(13), &pass) == VK_NULL_HANDLE, "Cached linker failure changed");
        pipeline.build(state(13), &pass);
        require(driver.count(state(13), &pass) == 1 && pipeline.size() == before_invalid + 1,
                "Real Metal failure was not cached exactly once");
    }
    require(driver.live() == 0 && other_driver.live() == 0, "Pinned pipeline destructor did not release cached PSOs");
    return progress;
}

static void exception_retry(MetalContext& metal, Totals& totals, NSMutableArray* results) {
    Driver driver(metal, totals); Manager manager; Pass pass; DriverGate blocked;
    const auto input = state(17);
    driver.throws[17] = 1; driver.block(input, &pass, blocked);
    {
        TestPipeline pipeline(driver, manager);
        auto owner = std::async(std::launch::async, [&] { @autoreleasepool {
            try { pipeline.get(input, &pass); } catch (const InjectedDriverFailure&) { return true; }
            return false;
        } });
        const bool entered = blocked.entered.wait();
        auto waiter = std::async(std::launch::async, [&] { @autoreleasepool { return pipeline.get(input, &pass); } });
        const bool waiting = driver.wait_for_waiters(input, &pass, 1);
        blocked.open();
        const bool expected_exception = owner.get(); const auto handle = waiter.get();
        require(entered && waiting && expected_exception && handle != VK_NULL_HANDLE && driver.count(input, &pass) == 2,
                "Injected post-Metal exception did not release ownership and wake retry");
        render(results, @"exception_retry", metal, driver, handle, 17);
    }
    require(driver.live() == 0, "Exception retry PSO ownership leaked");
}

static bool publication_case(MetalContext& metal, Totals& totals, NSMutableArray* results) {
    Driver driver(metal, totals); Manager manager; Pass pass; DriverGate destroying;
    const auto input = state(31);
    bool held = false;
    {
        TestPipeline pipeline(driver, manager);
        VkPipeline handle;
#if PROTOTYPE
        driver.allocation_failures[31] = 1; driver.destroy_gate = &destroying;
        auto owner = std::async(std::launch::async, [&] { @autoreleasepool {
            try { pipeline.get(input, &pass); } catch (const std::bad_alloc&) { return true; }
            return false;
        } });
        const bool cleanup_entered = destroying.entered.wait();
        auto waiter = std::async(std::launch::async, [&] { @autoreleasepool { return pipeline.get(input, &pass); } });
        const bool waiter_observed = driver.wait_for_waiters(input, &pass, 1);
        const unsigned calls = driver.count(input, &pass), retained = driver.live();
        const unsigned published = manager.m_numGraphicsPipelines;
        destroying.open();
        const bool expected_exception = owner.get(); handle = waiter.get();
        driver.destroy_gate = nullptr;
        held = cleanup_entered && waiter_observed && calls == 1 && retained == 0 && published == 0;
        require(held && expected_exception && handle != VK_NULL_HANDLE && driver.count(input, &pass) == 2,
                "Publication failure did not release the real PSO before releasing its flight");
#else
        handle = pipeline.get(input, &pass);
#endif
        render(results, @"publication_result", metal, driver, handle, 31);
    }
    require(driver.live() == 0, "Publication-case PSO ownership leaked");
    return held;
}

int main() {
    @autoreleasepool {
        try {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (device == nil) {
                std::puts("{\"schema\":1,\"status\":\"unavailable\",\"metal_execution_verified\":false}");
                return 77;
            }
            MetalContext metal(device); Totals totals;
            NSMutableArray* results = [NSMutableArray new];
            const bool progress = coordination_cases(metal, totals, results);
            exception_retry(metal, totals, results);
            const bool held = publication_case(metal, totals, results);
            const unsigned expected = PROTOTYPE ? 10 : 9;
            require(totals.created == expected && totals.released == expected && totals.live == 0,
                    "Every fixture-retained real Metal PSO must be released");
            require(totals.rejected == 1 && totals.exceptions == 1 && totals.publication_failures == unsigned(PROTOTYPE),
                    "Expected Metal/error injection coverage was incomplete");
            NSDictionary* report = @{
                @"schema":@1, @"status":@"passed",
                @"model":PROTOTYPE ? @"state_flights_prototype" : @"production_baseline",
                @"scope":@"mac-metal-driver-fixture", @"pso_ownership_scope":@"fixture-retained-references",
                @"public_only":@YES, @"metal_execution_verified":@YES,
                @"full_dxvk_moltenvk_verified":@NO, @"physical_iphone_verified":@NO,
                @"game_rendering_verified":@NO, @"performance_verified":@NO, @"production_enabled":@NO,
                @"distinct_state_progress":@(progress), @"publication_allocation_failure_exercised":@(bool(PROTOTYPE)),
                @"cleanup_kept_flight_owned":@(held),
                @"checks":@{@"ready_hit":@YES, @"independent_owner":@YES, @"same_identity_single_generation":@YES,
                    @"full_state_identity":@YES, @"render_pass_pointer_identity":@YES, @"real_metal_cached_null":@YES,
                    @"invalid_state_absent":@YES, @"exception_retry":@YES, @"all_pso_references_released":@YES},
                @"counts":@{@"pso_created":@(totals.created.load()), @"pso_released":@(totals.released.load()),
                    @"pso_live":@(totals.live.load()), @"pso_rejected":@(totals.rejected.load()),
                    @"injected_exceptions":@(totals.exceptions.load()), @"publication_failures":@(totals.publication_failures.load())},
                @"cases":results,
            };
            NSError* error = nil;
            NSData* json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
            metal_require(json != nil, error, "Metal evidence encoding failed");
            std::fwrite(json.bytes, 1, json.length, stdout); std::putchar('\n');
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Mac Metal graphics-flight validation failed: %s\n", error.what());
            return 2;
        }
    }
}
