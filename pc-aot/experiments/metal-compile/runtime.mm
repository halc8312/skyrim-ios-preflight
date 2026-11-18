// Standalone authored-library experiment. This is not a MoltenVK replacement.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>

#ifdef SKYRIM_METAL_RUNTIME_NO_MAIN
#import "runtime_host.h"
#import "phone_contention.h"
#include "phone_workloads.h"
#import <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/utsname.h>
#endif

#if TARGET_OS_OSX
#if !defined(__MAC_OS_X_VERSION_MAX_ALLOWED) || __MAC_OS_X_VERSION_MAX_ALLOWED < 260000
#error This fixture requires the macOS 26 SDK or later
#endif
#elif TARGET_OS_IOS && !TARGET_OS_MACCATALYST
#if !defined(__IPHONE_OS_VERSION_MAX_ALLOWED) || __IPHONE_OS_VERSION_MAX_ALLOWED < 260000
#error This fixture requires the iOS 26 SDK or later
#endif
#else
#error This fixture supports only the macOS or iOS SDK
#endif

#if TARGET_OS_SIMULATOR && !defined(SKYRIM_METAL_RUNTIME_NO_MAIN)
#error The simulator supports only the callable unsupported UI-preview path
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

using Clock = std::chrono::steady_clock;

static uint64_t elapsed(Clock::time_point start) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ProbeInterrupted final : std::exception {
    const char* what() const noexcept override { return "stopped before starting the next operation"; }
};

static void checkContinue(BOOL (^shouldContinue)(void)) {
    if (shouldContinue && !shouldContinue()) throw ProbeInterrupted();
}

// Record capability names and booleans before any eligibility guard. This
// distinguishes a missing/virtualized device from a device lacking a family.
// Do not include registry IDs or substitute a different device for the default.
static NSDictionary* deviceInfo(id<MTLDevice> device) {
    NSMutableDictionary* info = [@{
        @"available": @(device != nil), @"name": device.name ?: @"",
        @"supports_metal4": @NO, @"metal4_family_queried": @NO,
        @"supports_metal3": @NO, @"supports_mac2": @NO,
        @"supports_apple7": @NO, @"supports_apple8": @NO, @"supports_apple9": @NO,
        @"architecture_query_status": device ? @"unavailable_os" : @"no_device",
        @"architecture_name": NSNull.null,
        @"new_compiler_selector_available": @NO
    } mutableCopy];
    if (!device) return info;
#if !TARGET_OS_SIMULATOR
    info[@"new_compiler_selector_available"] = @([device respondsToSelector:@selector(newCompilerWithDescriptor:error:)]);
#endif
    if (@available(macOS 14.0, iOS 17.0, *)) {
        @try {
            NSString* architecture = device.architecture.name;
            info[@"architecture_query_status"] = architecture.length ? @"observed" : @"empty_result";
            info[@"architecture_name"] = architecture.length ? (id)architecture : NSNull.null;
        } @catch (NSException* error) {
            // Optional provenance must not turn an otherwise useful legacy
            // test into a guessed architecture or a different device choice.
            info[@"architecture_query_status"] = @"exception";
            info[@"architecture_query_exception"] = error.name ?: @"unknown";
        }
    }
    info[@"supports_metal3"] = @([device supportsFamily:MTLGPUFamilyMetal3]);
    info[@"supports_mac2"] = @([device supportsFamily:MTLGPUFamilyMac2]);
    info[@"supports_apple7"] = @([device supportsFamily:MTLGPUFamilyApple7]);
    info[@"supports_apple8"] = @([device supportsFamily:MTLGPUFamilyApple8]);
    info[@"supports_apple9"] = @([device supportsFamily:MTLGPUFamilyApple9]);
#if !TARGET_OS_SIMULATOR
    if (@available(macOS 26.0, iOS 26.0, *)) {
        info[@"metal4_family_queried"] = @YES;
        info[@"supports_metal4"] = @([device supportsFamily:MTLGPUFamilyMetal4]);
    }
#endif
    return info;
}

static void recordDevices(NSMutableDictionary* report, id<MTLDevice> device) {
    report[@"device"] = deviceInfo(device);
    report[@"default_device_available"] = @(device != nil);
    NSMutableArray* available = [NSMutableArray new];
#if TARGET_OS_OSX
    report[@"device_enumeration"] = @"all_devices";
    for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
        [available addObject:deviceInfo(candidate)];
    }
#else
    report[@"device_enumeration"] = @"default_device_only";
    if (device) [available addObject:deviceInfo(device)];
#endif
    report[@"available_devices"] = available;
    report[@"available_device_count"] = @(available.count);
}

#if !TARGET_OS_SIMULATOR
static NSDictionary* errorInfo(NSError* error) {
    return error ? @{ @"domain": error.domain, @"code": @(error.code),
                     @"description": error.localizedDescription ?: @"" } : @{};
}
#endif

static void requireObject(id object, NSError* error, const char* operation) {
    if (object) return; // A non-nil result may carry a compiler warning.
    std::string message(operation);
    message += ": ";
    message += error ? error.localizedDescription.UTF8String : "nil result without NSError";
    throw std::runtime_error(message);
}

static NSDictionary* sourceOptions() {
    return @{ @"language": @"2.4", @"fast_math": @NO,
              @"preprocessor_macros": @{ @"PROBE_BIAS": @3 },
              @"function_constant": @{ @"index": @0, @"type": @"uint", @"value": @7 } };
}

// The simulator SDK intentionally omits Metal 4 compiler declarations. Its UI
// preview still uses the real report/stop/unsupported path below, but cannot
// compile or execute these comparison routes.
#if !TARGET_OS_SIMULATOR
static MTLCompileOptions* compileOptions() {
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_4;
    // Keep the legacy switch and its documented safe/precise mapping identical.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    options.preprocessorMacros = @{ @"PROBE_BIAS": @3 };
    return options;
}

// This explicit diagnostic runs after the normal comparison has terminated,
// including its unsupported guard. It never creates a PSO or submits GPU work.
#ifndef SKYRIM_METAL_RUNTIME_NO_MAIN
static NSDictionary* probeLibraryFactory(id<MTLDevice> device, NSString* sourcePath) {
    NSMutableDictionary* result = [@{
        @"scope": @"metal4-factory-and-authored-library-only", @"status": @"not_attempted",
        @"factory_attempted": @NO, @"factory_created": @NO,
        @"library_attempted": @NO, @"library_created": @NO,
        @"gpu_execution_verified": @NO, @"full_metal4_verified": @NO,
        @"full_moltenvk_verified": @NO, @"physical_iphone_verified": @NO,
        @"game_rendering_verified": @NO, @"performance_improvement_verified": @NO,
        @"function_specialization_performed": @NO,
        @"render_pipeline_created": @NO, @"compute_pipeline_created": @NO
    } mutableCopy];
    if (@available(macOS 26.0, iOS 26.0, *)) {
        if (!device) { result[@"status"] = @"no_device"; return result; }
        if (![device respondsToSelector:@selector(newCompilerWithDescriptor:error:)]) {
            result[@"status"] = @"selector_unavailable";
            return result;
        }
        NSString* phase = @"factory";
        @try {
            NSError* error = nil;
            result[@"factory_attempted"] = @YES;
            printf("library_factory_probe phase=factory begin\n");
            fflush(stdout);
            id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
            result[@"factory_created"] = @(compiler != nil);
            result[@"factory_error_present"] = @(error != nil);
            result[@"factory_error"] = error ? @{ @"domain": error.domain, @"code": @(error.code) } : @{};
            if (!compiler) { result[@"status"] = @"factory_failed"; return result; }
            phase = @"source_read";
            error = nil;
            NSString* source = [NSString stringWithContentsOfFile:sourcePath encoding:NSUTF8StringEncoding error:&error];
            if (!source) {
                result[@"status"] = @"input_failed";
                result[@"input_error"] = error ? @{ @"domain": error.domain, @"code": @(error.code) } : @{};
                return result;
            }
            phase = @"library";
            MTL4LibraryDescriptor* descriptor = [MTL4LibraryDescriptor new];
            descriptor.source = source;
            descriptor.options = compileOptions();
            error = nil;
            result[@"library_attempted"] = @YES;
            printf("library_factory_probe phase=library begin\n");
            fflush(stdout);
            id<MTLLibrary> library = [compiler newLibraryWithDescriptor:descriptor error:&error];
            result[@"library_created"] = @(library != nil);
            result[@"library_error_present"] = @(error != nil);
            result[@"library_error"] = error ? @{ @"domain": error.domain, @"code": @(error.code) } : @{};
            result[@"status"] = library ? @"library_created" : @"library_failed";
        } @catch (NSException* exception) {
            result[@"status"] = @"exception";
            result[@"exception_phase"] = phase;
            result[@"exception_name"] = exception.name ?: @"unknown";
        }
    } else {
        result[@"status"] = @"os_unavailable";
    }
    return result;
}
#endif

static id<MTLLibrary> sourceLibrary(id<MTLDevice> device, id<MTL4Compiler> compiler,
                                   NSString* source, bool metal4, NSError** error,
                                   MTLCompileOptions* suppliedOptions = nil) {
    MTLCompileOptions* options = suppliedOptions ?: compileOptions();
    if (!metal4) return [device newLibraryWithSource:source options:options error:error];
    MTL4LibraryDescriptor* descriptor = [MTL4LibraryDescriptor new];
    descriptor.source = source;
    descriptor.options = options;
    return [compiler newLibraryWithDescriptor:descriptor error:error];
}

// Independent adapter experiment. No callback borrows the waiting thread's stack.
// The request owner keeps the task/compiler alive through completion. A timed
// wait only observes readiness; it neither releases that owner nor cancels Metal.
struct AsyncLibraryState {
    std::mutex mutex;
    std::condition_variable changed;
    NSString* source = nil;
    MTLCompileOptions* options = nil;
    MTL4LibraryDescriptor* descriptor = nil;
    id<MTLLibrary> library = nil;
    NSError* error = nil;
    unsigned callbacks = 0;
    bool received = false;
    bool published = false;
    bool holdPublication = false;
};

struct AsyncLibraryRequest {
    std::shared_ptr<AsyncLibraryState> state;
    id<MTLDevice> device = nil;
    id<MTL4Compiler> compiler = nil;
    id<MTL4CompilerTask> task = nil;

    void waitForCallback() const {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait(lock, [&] { return state->received; });
    }

    bool waitFor(std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(state->mutex);
        return state->changed.wait_for(lock, timeout, [&] { return state->published; });
    }

    void releasePublication() const {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->holdPublication = false;
        state->published = state->received;
        state->changed.notify_all();
    }

    void drain() const {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->changed.wait(lock, [&] { return state->published; });
        }
        // Do not hold the callback's mutex while joining Metal's task.
        if (task) [task waitUntilCompleted];
    }

    id<MTLLibrary> result(NSError** error, NSMutableDictionary* observation) const {
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->published && state->callbacks == 1, "async callback count/publication mismatch");
        *error = state->error;
        observation[@"async_callback_count"] = @(state->callbacks);
        observation[@"metal4_task_joined"] = @(task != nil);
        if (task) {
            require(task.status == MTL4CompilerTaskStatusFinished, "Metal 4 task was not finished after join");
            observation[@"metal4_task_status"] = @"finished";
        }
        return state->library;
    }
};

static AsyncLibraryRequest beginAsyncLibrary(id<MTLDevice> device, id<MTL4Compiler> compiler,
                                             NSString* source, MTLCompileOptions* options,
                                             bool metal4, bool holdPublication) {
    AsyncLibraryRequest request;
    request.state = std::make_shared<AsyncLibraryState>();
    request.device = device;
    request.compiler = compiler;
    const auto state = request.state;
    state->source = [source copy];
    state->options = [options copy];
    state->holdPublication = holdPublication;
    MTLNewLibraryCompletionHandler completion = ^(id<MTLLibrary> library, NSError* error) {
        @autoreleasepool {
            std::lock_guard<std::mutex> lock(state->mutex);
            ++state->callbacks;
            // ARC strong fields retain both results beyond this callback/pool.
            state->library = library;
            state->error = error;
            state->received = true;
            state->published = !state->holdPublication;
            state->changed.notify_all();
        }
    };
    if (metal4) {
        state->descriptor = [MTL4LibraryDescriptor new];
        state->descriptor.source = state->source;
        state->descriptor.options = state->options;
        request.task = [compiler newLibraryWithDescriptor:state->descriptor completionHandler:completion];
        requireObject(request.task, nil, "Metal 4 async task creation");
    } else {
        [device newLibraryWithSource:state->source options:state->options completionHandler:completion];
    }
    return request;
}

static id<MTLLibrary> asyncSourceLibrary(id<MTLDevice> device, id<MTL4Compiler> compiler,
                                        NSString* source, bool metal4, NSError** error,
                                         NSMutableDictionary* observation,
                                         MTLCompileOptions* suppliedOptions = nil) {
    auto request = beginAsyncLibrary(device, compiler, source, suppliedOptions ?: compileOptions(), metal4, false);
    request.drain();
    return request.result(error, observation);
}

static NSArray* words(const uint32_t* data, size_t count) {
    NSMutableArray* values = [NSMutableArray arrayWithCapacity:count];
    for (size_t i = 0; i < count; ++i) [values addObject:@(data[i])];
    return values;
}

static void runGPU(id<MTLDevice> device, id<MTLCommandQueue> queue,
                   id<MTLLibrary> library, NSMutableDictionary* sample,
                   BOOL (^shouldContinue)(void) = nil) {
    checkContinue(shouldContinue);
    // Loading a valid container does not establish that entry points survived linking.
    sample[@"library_function_names"] = [library.functionNames sortedArrayUsingSelector:@selector(compare:)] ?: @[];
    NSError* error = nil;
    auto start = Clock::now();
    id<MTLFunction> vertex = [library newFunctionWithName:@"probe_vertex"];
    requireObject(vertex, nil, "vertex function lookup");
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    uint32_t scale = 7;
    [constants setConstantValue:&scale type:MTLDataTypeUInt atIndex:0];
    id<MTLFunction> fragment = [library newFunctionWithName:@"probe_fragment"
                                          constantValues:constants error:&error];
    sample[@"fragment_diagnostic"] = errorInfo(error);
    requireObject(fragment, error, "fragment specialization");
    checkContinue(shouldContinue);
    error = nil;
    id<MTLFunction> kernel = [library newFunctionWithName:@"probe_kernel"
                                        constantValues:constants error:&error];
    sample[@"compute_function_diagnostic"] = errorInfo(error);
    requireObject(kernel, error, "compute specialization");
    sample[@"specialization_ns"] = @(elapsed(start));

    checkContinue(shouldContinue);

    MTLRenderPipelineDescriptor* renderDescriptor = [MTLRenderPipelineDescriptor new];
    renderDescriptor.vertexFunction = vertex;
    renderDescriptor.fragmentFunction = fragment;
    renderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    error = nil;
    start = Clock::now();
    id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:renderDescriptor error:&error];
    sample[@"render_pso_ns"] = @(elapsed(start));
    sample[@"render_pso_diagnostic"] = errorInfo(error);
    requireObject(render, error, "legacy render pipeline");
    checkContinue(shouldContinue);
    error = nil;
    start = Clock::now();
    id<MTLComputePipelineState> compute = [device newComputePipelineStateWithFunction:kernel error:&error];
    sample[@"compute_pso_ns"] = @(elapsed(start));
    sample[@"compute_pso_diagnostic"] = errorInfo(error);
    requireObject(compute, error, "legacy compute pipeline");

    MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                              width:4 height:4 mipmapped:NO];
    textureDescriptor.storageMode = MTLStorageModeShared;
    textureDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
    id<MTLBuffer> output = [device newBufferWithLength:16 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    requireObject(texture, nil, "readback texture");
    requireObject(output, nil, "readback buffer");
    memset(output.contents, 0xcd, output.length);

    id<MTLCommandBuffer> command = [queue commandBuffer];
    requireObject(command, nil, "legacy command buffer");
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(-1, -1, -1, -1);
    id<MTLRenderCommandEncoder> drawing = [command renderCommandEncoderWithDescriptor:pass];
    requireObject(drawing, nil, "legacy render encoder");
    [drawing setRenderPipelineState:render];
    [drawing setViewport:MTLViewport{0, 0, 4, 4, 0, 1}];
    [drawing drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [drawing endEncoding];
    id<MTLComputeCommandEncoder> computing = [command computeCommandEncoder];
    requireObject(computing, nil, "legacy compute encoder");
    [computing setComputePipelineState:compute];
    [computing setBuffer:output offset:0 atIndex:0];
    [computing dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
    [computing endEncoding];
    // A phone stop/background request only prevents this submission. Once
    // committed, completion still drains on the worker; there is no cancellation.
    checkContinue(shouldContinue);
    [command commit];
    // The CLI supervisor has a deadline. This API itself has no timeout; the
    // UIKit host keeps this wait off its main thread and never pretends to cancel.
    [command waitUntilCompleted];
    sample[@"command_status_value"] = @(command.status);
    sample[@"command_error"] = errorInfo(command.error);
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
            "GPU command did not complete successfully");

    std::array<float, 64> pixels{};
    [texture getBytes:pixels.data() bytesPerRow:4 * 4 * sizeof(float)
          fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    std::array<uint32_t, 64> pixelBits{};
    memcpy(pixelBits.data(), pixels.data(), sizeof(pixels));
    std::array<uint32_t, 16> computeWords{};
    memcpy(computeWords.data(), output.contents, sizeof(computeWords));
    sample[@"render_bits"] = words(pixelBits.data(), pixelBits.size());
    sample[@"compute_words"] = words(computeWords.data(), computeWords.size());
    sample[@"command_status"] = @"completed";
    const float color[4] = {3.0f / 16.0f, 7.0f / 16.0f, 0.5f, 1.0f};
    for (size_t i = 0; i < pixels.size(); ++i)
        require(pixels[i] == color[i % 4], "render readback differs from the authored expected color");
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t expected[4] = {i + 3, (i + 1) * 7, 10, 0x12345678u};
        for (size_t j = 0; j < 4; ++j)
            require(computeWords[i * 4 + j] == expected[j], "compute readback differs from authored expected words");
    }
}

static void checkAsyncRetention(id<MTLDevice> device, id<MTL4Compiler> compiler,
                                id<MTLCommandQueue> queue, NSString* source,
                                bool metal4, bool invalid, NSMutableArray* observations,
                                BOOL (^shouldContinue)(void) = nil) {
    checkContinue(shouldContinue);
    NSMutableDictionary* observation = [@{
        @"api": metal4 ? @"metal4_async_source" : @"legacy_async_source",
        @"case": invalid ? @"compiler_error" : @"library_success",
        @"timeout_scope": @"controlled publication gate after the real compiler callback; not compilation latency",
        @"cancellation_requested": @NO
    } mutableCopy];
    [observations addObject:observation];
    AsyncLibraryRequest request;
    @autoreleasepool {
        NSMutableString* callerSource = [source mutableCopy];
        MTLCompileOptions* callerOptions = compileOptions();
        request = beginAsyncLibrary(device, compiler, callerSource, callerOptions, metal4, true);
        // The adapter must own its inputs, rather than use mutable caller storage.
        [callerSource appendString:@"\n#error caller_source_changed_after_submission\n"];
        callerOptions.preprocessorMacros = @{ @"PROBE_BIAS": @99 };
        callerOptions.languageVersion = MTLLanguageVersion2_3;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        callerOptions.fastMathEnabled = YES;
        bool snapshotMatches = !request.state->options.fastMathEnabled;
#pragma clang diagnostic pop
        snapshotMatches = snapshotMatches && [request.state->source isEqual:source]
            && request.state->options.languageVersion == MTLLanguageVersion2_4
            && [request.state->options.preprocessorMacros isEqual:@{ @"PROBE_BIAS": @3 }];
        observation[@"input_snapshot_verified"] = @(snapshotMatches);
        require(snapshotMatches, "async input snapshot changed with caller inputs");
    }
    request.waitForCallback();
    // This deterministic wait failure tests retained state, not compiler speed.
    bool ready = request.waitFor(std::chrono::milliseconds(0));
    observation[@"wait_timed_out"] = @(!ready);
    require(!ready, "controlled async publication escaped its gate");
    request.releasePublication();
    request.drain();
    checkContinue(shouldContinue);
    NSError* error = nil;
    id<MTLLibrary> library = request.result(&error, observation);
    observation[@"returned_nil"] = @(library == nil);
    observation[@"error_present"] = @(error != nil);
    observation[@"error"] = errorInfo(error);
    if (invalid) {
        require(!library && error, "late async rejection lost nil library or NSError");
    } else {
        requireObject(library, error, "late async library");
        runGPU(device, queue, library, observation, shouldContinue);
    }
}
#endif

#ifndef SKYRIM_METAL_RUNTIME_NO_MAIN
static void writeReport(NSDictionary* report, NSString* path) {
    NSError* error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
    requireObject(data, error, "report serialization");
    require([data writeToFile:path options:NSDataWritingAtomic error:&error], "could not write native report");
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSString* sourcePath = nil;
        NSString* reportPath = nil;
        NSString* libraryPath = nil;
        unsigned repeats = 0;
        bool legacyOnly = false;
        bool asyncLibrary = false;
        bool libraryFactoryProbe = false;
        bool argumentsValid = false;
        id<MTLDevice> device = nil;
        NSMutableDictionary* report = [@{
            @"schema": @1, @"status": @"failed", @"scope": @"authored-metal-library-api-and-readback",
            @"source_options": sourceOptions(), @"gpu_execution_verified": @NO,
            @"full_metal4_verified": @NO, @"metal4_library_verified": @NO,
            @"metal4_compiler_created": @NO, @"compiler_setup_ns": @0,
            @"full_moltenvk_verified": @NO, @"physical_iphone_verified": @NO,
            @"game_rendering_verified": @NO, @"performance_improvement_verified": @NO,
            @"cold_os_cache_verified": @NO,
            @"specialization_scope": @"vertex lookup plus fragment and compute specialization",
            @"offline_compile_options_verified": @NO,
            @"production_async_lifetime_verified": @NO,
            @"async_adapter_verified": @NO, @"metal4_async_adapter_verified": @NO,
            @"samples": [NSMutableArray new], @"rejections": [NSMutableArray new],
            @"async_retention_checks": [NSMutableArray new]
        } mutableCopy];
        int result = 1;
        try {
            for (int i = 1; i < argc; ++i) {
                const std::string key(argv[i]);
                if (key == "--legacy-only" && !legacyOnly) {
                    legacyOnly = true;
                    continue;
                }
                if (key == "--async-library" && !asyncLibrary) {
                    asyncLibrary = true;
                    continue;
                }
                if (key == "--probe-library-factory" && !libraryFactoryProbe) {
                    libraryFactoryProbe = true;
                    continue;
                }
                require(i + 1 < argc, "missing command argument");
                const char* argument = argv[++i];
                NSString* value = [NSString stringWithUTF8String:argument];
                require(value != nil, "non-UTF8 argument");
                if (key == "--source" && !sourcePath) sourcePath = value;
                else if (key == "--report" && !reportPath) reportPath = value;
                else if (key == "--library" && !libraryPath) libraryPath = value;
                else if (key == "--repeat" && !repeats) {
                    require(strlen(argument) == 1 && argument[0] >= '1' && argument[0] <= '5',
                            "repeat must be between 1 and 5");
                    repeats = unsigned(argument[0] - '0');
                } else throw std::runtime_error("unknown or duplicate argument");
            }
            require(sourcePath && reportPath && repeats, "source, report and repeat are required");
            require(!legacyOnly || libraryPath, "legacy-only requires an offline library");
            require(!legacyOnly || !libraryFactoryProbe,
                    "legacy-only cannot request the Metal 4 factory probe");
            argumentsValid = true;
            report[@"comparison_mode"] = legacyOnly ? @"legacy-only" : @"metal4-comparison";
            report[@"legacy_only_requested"] = @(legacyOnly);
            report[@"repeat"] = @(repeats);
            report[@"offline_library_requested"] = @(libraryPath != nil);
            report[@"async_library_requested"] = @(asyncLibrary);
            report[@"library_factory_probe_requested"] = @(libraryFactoryProbe);
            NSArray<NSString*>* asyncAPIs = !asyncLibrary ? @[] : legacyOnly
                ? @[@"legacy_async_source"] : @[@"legacy_async_source", @"metal4_async_source"];
            report[@"async_apis"] = asyncAPIs;
            report[@"async_timing_scope"] = legacyOnly
                ? @"submission through callback publication; controlled retention checks excluded"
                : @"submission through callback publication and task join; controlled retention checks excluded";
            NSOperatingSystemVersion version = NSProcessInfo.processInfo.operatingSystemVersion;
            report[@"os_version"] = @[@(version.majorVersion), @(version.minorVersion), @(version.patchVersion)];
            device = MTLCreateSystemDefaultDevice();
            recordDevices(report, device);
            if (@available(macOS 26.0, iOS 26.0, *)) {
                if (!device) {
                    report[@"status"] = @"unsupported";
                    report[@"unsupported_reason"] = @"no_default_metal_device";
                    throw std::runtime_error("no default Metal device is available");
                }
                if (!legacyOnly && ![report[@"device"][@"supports_metal4"] boolValue]) {
                    report[@"status"] = @"unsupported";
                    report[@"unsupported_reason"] = @"metal4_family_unavailable";
                    throw std::runtime_error("a Metal 4 capable device is required");
                }
                if (!legacyOnly && ![report[@"device"][@"new_compiler_selector_available"] boolValue]) {
                    report[@"status"] = @"unsupported";
                    report[@"unsupported_reason"] = @"metal4_compiler_selector_unavailable";
                    throw std::runtime_error("the Metal 4 compiler factory is unavailable");
                }
            } else {
                report[@"status"] = @"unsupported";
                report[@"unsupported_reason"] = @"os_before_26";
                throw std::runtime_error("macOS 26 or iOS 26 or later is required");
            }
            NSError* error = nil;
            NSString* source = [NSString stringWithContentsOfFile:sourcePath encoding:NSUTF8StringEncoding error:&error];
            requireObject(source, error, "authored source read");
            auto start = Clock::now();
            id<MTL4Compiler> compiler = nil;
            if (!legacyOnly) {
                error = nil;
                compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
                report[@"compiler_setup_ns"] = @(elapsed(start));
                report[@"metal4_compiler_created"] = @(compiler != nil);
                requireObject(compiler, error, "Metal 4 compiler creation");
            }
            id<MTLCommandQueue> queue = [device newCommandQueue];
            requireObject(queue, nil, "legacy command queue");
            NSMutableArray* samples = report[@"samples"];
            NSMutableArray<NSString*>* sourceAPIs = [NSMutableArray arrayWithObject:@"legacy_source"];
            if (!legacyOnly) [sourceAPIs addObject:@"metal4_source"];
            [sourceAPIs addObjectsFromArray:asyncAPIs];
            unsigned sourceRoutes = unsigned(sourceAPIs.count);
            for (unsigned iteration = 0; iteration < repeats; ++iteration) {
                // Rotate the source routes. OS/compiler caches remain uncontrolled.
                for (unsigned slot = 0; slot < sourceRoutes + (libraryPath ? 1u : 0u); ++slot) {
                    @autoreleasepool {
                        bool offline = slot == sourceRoutes;
                        unsigned route = (slot + iteration) % sourceRoutes;
                        NSString* api = offline ? @"offline_metallib" : sourceAPIs[route];
                        bool metal4 = [api hasPrefix:@"metal4_"];
                        bool asynchronous = [api containsString:@"_async_"];
                        NSMutableDictionary* sample = [@{ @"api": api, @"iteration": @(iteration + 1),
                            @"library_operation": offline ? @"load" : asynchronous ? @"async_compile" : @"compile" } mutableCopy];
                        [samples addObject:sample];
                        printf("iteration=%u api=%s library begin\n", iteration + 1, api.UTF8String);
                        fflush(stdout);
                        error = nil;
                        start = Clock::now();
                        id<MTLLibrary> library = offline
                            ? [device newLibraryWithURL:[NSURL fileURLWithPath:libraryPath] error:&error]
                            : asynchronous ? asyncSourceLibrary(device, compiler, source, metal4, &error, sample)
                                           : sourceLibrary(device, compiler, source, metal4, &error);
                        sample[@"library_ns"] = @(elapsed(start));
                        sample[@"library_diagnostic"] = errorInfo(error);
                        requireObject(library, error, "library creation");
                        runGPU(device, queue, library, sample);
                        printf("iteration=%u api=%s readback verified\n", iteration + 1, api.UTF8String);
                        fflush(stdout);
                    }
                }
            }
            NSString* invalidSource = [@"#error Intentional_Metal_compile_fixture_rejection\n" stringByAppendingString:source];
            NSMutableArray* rejections = report[@"rejections"];
            for (NSString* api in sourceAPIs) {
                error = nil;
                bool metal4 = [api hasPrefix:@"metal4_"];
                bool asynchronous = [api containsString:@"_async_"];
                NSMutableDictionary* rejection = [@{ @"api": api } mutableCopy];
                [rejections addObject:rejection];
                start = Clock::now();
                id<MTLLibrary> invalid = asynchronous
                    ? asyncSourceLibrary(device, compiler, invalidSource, metal4, &error, rejection)
                    : sourceLibrary(device, compiler, invalidSource, metal4, &error);
                [rejection addEntriesFromDictionary:@{ @"library_ns": @(elapsed(start)), @"returned_nil": @(invalid == nil),
                                                       @"error_present": @(error != nil), @"error": errorInfo(error) }];
                require(!invalid && error, "invalid source did not produce nil library plus NSError");
            }
            if (asyncLibrary) {
                for (NSString* api in asyncAPIs) {
                    bool metal4 = [api hasPrefix:@"metal4_"];
                    checkAsyncRetention(device, compiler, queue, source, metal4, false, report[@"async_retention_checks"]);
                    checkAsyncRetention(device, compiler, queue, invalidSource, metal4, true, report[@"async_retention_checks"]);
                }
                report[@"async_adapter_verified"] = @YES;
                report[@"metal4_async_adapter_verified"] = @(!legacyOnly);
            }
            report[@"gpu_execution_verified"] = @YES;
            report[@"metal4_library_verified"] = @(!legacyOnly);
            report[@"status"] = @"passed";
            result = 0;
        } catch (const std::exception& exception) {
            report[@"failure"] = [NSString stringWithUTF8String:exception.what()] ?: @"native failure";
            fprintf(stderr, "%s\n", exception.what());
            result = [report[@"status"] isEqual:@"unsupported"] ? 78 : 1;
        }
        if (argumentsValid && libraryFactoryProbe) {
            // A successful narrow probe does not alter the normal status/exit.
            // Preserve the normal diagnostics even if this opt-in API probe
            // later stalls or the driver terminates the process.
            report[@"library_factory_probe"] = @{
                @"scope": @"metal4-factory-and-authored-library-only", @"status": @"pending" };
            try { writeReport(report, reportPath); }
            catch (const std::exception& exception) {
                fprintf(stderr, "%s\n", exception.what());
                return 1;
            }
            report[@"library_factory_probe"] = probeLibraryFactory(device, sourcePath);
            printf("library_factory_probe status=%s; no GPU execution\n",
                   [report[@"library_factory_probe"][@"status"] UTF8String]);
            fflush(stdout);
        }
        if (reportPath) {
            try { writeReport(report, reportPath); }
            catch (const std::exception& exception) {
                fprintf(stderr, "%s\n", exception.what());
                result = 1;
            }
        }
        return result;
    }
}
#endif

#ifdef SKYRIM_METAL_RUNTIME_NO_MAIN
static bool phoneDisplayDuration(id value) {
    return [value isKindOfClass:NSNumber.class] &&
        CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID() &&
        std::isfinite([value doubleValue]) && [value doubleValue] >= 0;
}

static double phoneMedianMilliseconds(NSArray<NSNumber*>* values) {
    NSArray<NSNumber*>* sorted = [values sortedArrayUsingSelector:@selector(compare:)];
    const NSUInteger middle = sorted.count / 2;
    double value = sorted[middle].doubleValue;
    if (sorted.count % 2 == 0) value = (value + sorted[middle - 1].doubleValue) / 2;
    return value / 1e6;
}

NSString* SkyrimMetalPhoneContentionResultText(NSDictionary* comparison) {
    const NSUInteger schema = [comparison[@"schema"] unsignedIntegerValue];
    if (schema < 4) return nil;
    NSDictionary* control = [comparison[@"contention_control"] isKindOfClass:NSDictionary.class]
        ? comparison[@"contention_control"] : @{};
    NSString* status = control[@"status"];
    NSString* summary = @"未実施";
    if ([status isEqual:@"passed"]) summary = @"記録済み";
    else if ([status isEqual:@"incomplete"] || [status isEqual:@"running"]) summary = @"未完了";
    else if ([status isEqual:@"stopped"] || [status isEqual:@"interrupted"]) summary = @"途中で停止";
    else if ([status isEqual:@"unsupported"]) summary = @"この環境では未対応";
    else if ([status isEqual:@"failed"]) summary = @"確認が必要な結果";
    NSMutableArray<NSString*>* lines = [NSMutableArray arrayWithObject:
        [NSString stringWithFormat:@"先読み中の待ち時間：%@", summary]];
    if (![status isEqual:@"passed"]) return [lines componentsJoinedByString:@"\n"];
    NSArray* trials = [control[@"trials"] isKindOfClass:NSArray.class] ? control[@"trials"] : @[];
    if (schema >= 5) {
        // Use every planned observation, not only requests which overlapped:
        // fast completions can remove overlap and must not leave the sample.
        if (![control[@"schema"] isEqual:@2] || trials.count != 32)
            return @"先読み中の待ち時間：記録の確認が必要です";
        [lines addObject:@"各8回の中央値・準備開始から完了通知まで"];
        for (NSString* api in @[@"legacy_async", @"metal4_async"]) {
            NSMutableArray<NSNumber*>* idle = [NSMutableArray new];
            NSMutableArray<NSNumber*>* planned = [NSMutableArray new];
            NSUInteger overlapping = 0;
            for (id value in trials) {
                if (![value isKindOfClass:NSDictionary.class])
                    return @"先読み中の待ち時間：記録の確認が必要です";
                NSDictionary* trial = value;
                if (![trial[@"api"] isEqual:api]) continue;
                NSNumber* duration = trial[@"urgent_request_to_callback_ns"];
                if (![trial[@"status"] isEqual:@"passed"] ||
                    ![trial[@"first_assigned_in_run"] isEqual:@YES] || !phoneDisplayDuration(duration))
                    return @"先読み中の待ち時間：記録の確認が必要です";
                if ([trial[@"mode"] isEqual:@"idle"]) [idle addObject:duration];
                else if ([trial[@"mode"] isEqual:@"contended"]) {
                    id observed = trial[@"host_overlap_observed"];
                    if (![observed isEqual:@YES] && ![observed isEqual:@NO])
                        return @"先読み中の待ち時間：記録の確認が必要です";
                    [planned addObject:duration];
                    overlapping += [observed boolValue] ? 1 : 0;
                } else return @"先読み中の待ち時間：記録の確認が必要です";
            }
            if (idle.count != 8 || planned.count != 8)
                return @"先読み中の待ち時間：記録の確認が必要です";
            [lines addObject:[NSString stringWithFormat:@"\n%@\n単独 %.2f ms ／ 先読みあり %.2f ms\n実際の重なり %lu / 8回%@",
                [api isEqual:@"legacy_async"] ? @"従来方式" : @"Metal 4",
                phoneMedianMilliseconds(idle), phoneMedianMilliseconds(planned),
                (unsigned long)overlapping, overlapping ? @"" : @"（未観測）"]];
        }
    } else {
        // Build 4 used repeated banks and reported an enqueue-to-callback max.
        // Keep that meaning when reopening a historical record.
        NSUInteger measured = 0, prefetchPending = 0;
        NSMutableDictionary<NSString*, NSNumber*>* maxima = [NSMutableDictionary new];
        for (id value in trials) {
            if (![value isKindOfClass:NSDictionary.class]) continue;
            NSDictionary* trial = value;
            if (![trial[@"status"] isEqual:@"passed"] || ![trial[@"mode"] isEqual:@"contended"] ||
                !phoneDisplayDuration(trial[@"urgent_enqueue_to_callback_ns"]) ||
                ![trial[@"prefetch_inflight_at_urgent_api_enter"] isKindOfClass:NSNumber.class]) continue;
            NSString* api = trial[@"api"];
            if (![api isEqual:@"legacy_async"] && ![api isEqual:@"metal4_async"]) continue;
            ++measured;
            if ([trial[@"prefetch_inflight_at_urgent_api_enter"] unsignedIntegerValue] > 0) ++prefetchPending;
            NSNumber* duration = trial[@"urgent_enqueue_to_callback_ns"];
            if (!maxima[api] || duration.unsignedLongLongValue > maxima[api].unsignedLongLongValue)
                maxima[api] = duration;
        }
        if (measured) {
            [lines addObject:@"以前の比較：最大値・開始待ちを含む"];
            for (NSString* api in @[@"legacy_async", @"metal4_async"])
                if (maxima[api]) [lines addObject:[NSString stringWithFormat:@"%@　%.2f ms",
                    [api isEqual:@"legacy_async"] ? @"従来" : @"Metal 4", maxima[api].doubleValue / 1e6]];
            [lines addObject:[NSString stringWithFormat:@"開始時に先読みが未完了：%lu / %lu回",
                (unsigned long)prefetchPending, (unsigned long)measured]];
        }
    }
    return [lines componentsJoinedByString:@"\n"];
}

static NSDictionary* phoneEnvironment() {
    NSMutableDictionary* result = [@{
        @"thermal_state": @(NSProcessInfo.processInfo.thermalState),
        @"low_power_mode": @(NSProcessInfo.processInfo.lowPowerModeEnabled),
        @"phys_footprint_bytes": NSNull.null,
        @"available_memory_bytes": NSNull.null,
        @"memory_scope": @"app footprint and advisory process headroom; not GPU-only memory or a hard allocation budget"
    } mutableCopy];
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t status = task_info(mach_task_self(), TASK_VM_INFO,
                                    reinterpret_cast<task_info_t>(&info), &count);
    result[@"footprint_query_status"] = @(status);
    if (status == KERN_SUCCESS && count >= TASK_VM_INFO_REV1_COUNT)
        result[@"phys_footprint_bytes"] = @(info.phys_footprint);
#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR && !TARGET_OS_MACCATALYST
    result[@"available_memory_bytes"] = @(os_proc_available_memory());
#endif
    return result;
}

static NSDictionary* phoneSnapshot(NSDictionary* report) {
    NSError* error = nil;
    NSData* encoded = [NSJSONSerialization dataWithJSONObject:report options:0 error:&error];
    requireObject(encoded, error, "phone report serialization");
    error = nil;
    NSDictionary* snapshot = [NSJSONSerialization JSONObjectWithData:encoded options:0 error:&error];
    requireObject(snapshot, error, "phone report snapshot");
    return snapshot;
}

static NSMutableDictionary* phoneProgressTiming() {
    return [@{
        @"scope": @"phoneCheckpoint wall intervals for callbacks that returned normally; final return snapshot excluded; intervals are outside library/specialization/PSO clocks",
        @"snapshot_scope": @"JSON encode plus immutable JSON decode; snapshot contains prior completed checkpoint totals",
        @"callback_scope": @"synchronous progress callback including caller storage and UI enqueue; nested host storage time must not be added again",
        @"considered_count": @0, @"completed_count": @0, @"skipped_count": @0, @"incomplete_count": @0,
        @"count_scope": @"considered after entry stop guard; completed is normal callback return; skipped is coalesced or nil callback; incomplete has not returned normally, including the current callback in its immutable snapshot; a final nonzero value is an aborted attempt; considered=completed+skipped+incomplete",
        @"policy": @"full snapshot every four completed serial samples, each case start and all control boundaries; no deep JSON or callback at skipped boundaries; stop guards still run before and after every boundary",
        @"serial_snapshot_stride": @(SkyrimPhoneProgress::SnapshotStride),
        @"recovery_scope": @"forced kill before a successful checkpoint may lose up to four completed serial samples plus unfinished work; separate contention control is recovered only after return; terminal stop/error returns all in-memory evidence; persistence requires a successful host callback",
        @"snapshot_serialization": @{ @"count": @0, @"total_ns": @0, @"max_ns": @0 },
        @"synchronous_callback": @{ @"count": @0, @"total_ns": @0, @"max_ns": @0 }
    } mutableCopy];
}

static void phoneRecordProgressInterval(NSMutableDictionary* report, NSString* key, uint64_t duration) {
    NSMutableDictionary* timing = report[@"progress_timing"];
    NSDictionary* previous = timing[key];
    const uint64_t priorMax = [previous[@"max_ns"] unsignedLongLongValue];
    timing[key] = @{ @"count": @([previous[@"count"] unsignedLongLongValue] + 1),
        @"total_ns": @([previous[@"total_ns"] unsignedLongLongValue] + duration),
        @"max_ns": @(duration > priorMax ? duration : priorMax) };
}

static void phoneCheckpoint(NSMutableDictionary* report, NSString* phase,
                            BOOL (^shouldContinue)(void), void (^progress)(NSDictionary*)) {
    report[@"phase"] = phase;
    checkContinue(shouldContinue);
    NSMutableDictionary* timing = report[@"progress_timing"];
    timing[@"considered_count"] = @([timing[@"considered_count"] unsignedLongLongValue] + 1);
    SkyrimPhoneProgress::Boundary boundary = SkyrimPhoneProgress::Boundary::Control;
    if ([phase isEqual:@"library"] || [phase isEqual:@"gpu_readback"])
        boundary = SkyrimPhoneProgress::Boundary::Operation;
    else if ([phase isEqual:@"sample_complete"])
        boundary = SkyrimPhoneProgress::Boundary::SampleComplete;
    const bool emit = SkyrimPhoneProgress::fullSnapshot(boundary, [report[@"completed_samples"] unsignedLongLongValue]);
    if (progress && emit) {
        timing[@"incomplete_count"] = @([timing[@"incomplete_count"] unsignedLongLongValue] + 1);
        const auto snapshotStarted = Clock::now();
        NSDictionary* snapshot = phoneSnapshot(report);
        const uint64_t snapshotNs = elapsed(snapshotStarted);
        const auto callbackStarted = Clock::now();
        progress(snapshot);
        const uint64_t callbackNs = elapsed(callbackStarted);
        // A throwing callback is not a completed progress call. Publish both
        // independent interval counters only after it returned normally.
        phoneRecordProgressInterval(report, @"snapshot_serialization", snapshotNs);
        phoneRecordProgressInterval(report, @"synchronous_callback", callbackNs);
        timing[@"completed_count"] = @([timing[@"completed_count"] unsignedLongLongValue] + 1);
        timing[@"incomplete_count"] = @([timing[@"incomplete_count"] unsignedLongLongValue] - 1);
    } else {
        timing[@"skipped_count"] = @([timing[@"skipped_count"] unsignedLongLongValue] + 1);
    }
    // Persistence may take time or request a stop; recheck before issuing work.
    checkContinue(shouldContinue);
}

#if !TARGET_OS_SIMULATOR
static NSString* phoneSourceHash(NSData* source) {
    require(source.length > 0 && source.length <= 64 * 1024, "authored source size is outside the probe limit");
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(source.bytes, static_cast<CC_LONG>(source.length), digest);
    NSMutableString* value = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (unsigned char byte : digest) [value appendFormat:@"%02x", byte];
    return value;
}

static NSDictionary* phoneWorkloadOptions(unsigned index) {
    return @{ @"language": @"2.4", @"fast_math": @NO,
        @"preprocessor_macros": @{ @"PHONE_CASE": @(SkyrimPhoneWorkloads::familyIndex(index)),
            @"PHONE_VARIANT": @(SkyrimPhoneWorkloads::variantIndex(index)),
            @"PHONE_ROUNDS": @(SkyrimPhoneWorkloads::rounds(index)) },
        @"function_constant": @{ @"index": @0, @"type": @"uint", @"value": @(SkyrimPhoneWorkloads::scale(index)) } };
}

static MTLCompileOptions* phoneCompileOptions(unsigned index) {
    MTLCompileOptions* options = compileOptions();
    options.preprocessorMacros = phoneWorkloadOptions(index)[@"preprocessor_macros"];
    return options;
}

template<size_t N>
static NSData* phoneWordData(const std::array<uint32_t, N>& input) {
    const auto bytes = SkyrimPhoneWorkloads::littleEndianBytes(input);
    return [NSData dataWithBytes:bytes.data() length:bytes.size()];
}

static NSDictionary* phoneWorkloadInfo(unsigned index, NSString* sourceHash, NSString* firstAPI = @"legacy_source") {
    using namespace SkyrimPhoneWorkloads;
    const Input input = makeInput(index);
    NSData* data = phoneWordData(input);
    NSDictionary* options = phoneWorkloadOptions(index);
    NSError* error = nil;
    NSData* optionsData = [NSJSONSerialization dataWithJSONObject:options options:NSJSONWritingSortedKeys error:&error];
    requireObject(optionsData, error, "authored options fingerprint");
    return @{ @"case_id": [NSString stringWithUTF8String:caseID(index)], @"case_index": @(index + 1),
        @"family_id": [NSString stringWithUTF8String:familyID(index)], @"family_index": @(familyIndex(index) + 1),
        @"variant_index": @(variantIndex(index) + 1), @"variant_salt": @(variantSalt(index)),
        @"case_first_api": firstAPI,
        @"source_sha256": sourceHash, @"source_options": options,
        @"options_sha256": phoneSourceHash(optionsData), @"options_fingerprint_format": @"sorted-key JSON UTF-8",
        @"input_sha256": phoneSourceHash(data), @"input_bytes": @(data.length),
        @"input_format": @"128 little-endian uint32 words; fixed authored seed per case",
        @"render_oracle_sha256": phoneSourceHash(phoneWordData(oracle(index, input, RenderTag))),
        @"compute_oracle_sha256": phoneSourceHash(phoneWordData(oracle(index, input, ComputeTag))),
        @"readback_format": @"RGBA32Uint render and uint4 compute; 64 words per stage",
        @"unrolled_rounds": @(rounds(index)) };
}

static void runPhoneWorkloadGPU(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                 id<MTLLibrary> library, unsigned index,
                                 const SkyrimPhoneWorkloads::Input& input,
                                 NSMutableDictionary* sample,
                                 BOOL (^shouldContinue)(void) = nil) {
    using namespace SkyrimPhoneWorkloads;
    checkContinue(shouldContinue);
    // Oracle generation is outside every measured compiler interval.
    const Output expectedRender = oracle(index, input, RenderTag);
    const Output expectedCompute = oracle(index, input, ComputeTag);
    sample[@"library_function_names"] = [library.functionNames sortedArrayUsingSelector:@selector(compare:)] ?: @[];
    NSError* error = nil;
    auto start = Clock::now();
    id<MTLFunction> vertex = [library newFunctionWithName:@"phone_vertex"];
    requireObject(vertex, nil, "phone vertex lookup");
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    uint32_t factor = scale(index);
    [constants setConstantValue:&factor type:MTLDataTypeUInt atIndex:0];
    id<MTLFunction> fragment = [library newFunctionWithName:@"phone_fragment" constantValues:constants error:&error];
    sample[@"fragment_diagnostic"] = errorInfo(error);
    requireObject(fragment, error, "phone fragment specialization");
    checkContinue(shouldContinue);
    error = nil;
    id<MTLFunction> kernel = [library newFunctionWithName:@"phone_kernel" constantValues:constants error:&error];
    sample[@"compute_function_diagnostic"] = errorInfo(error);
    requireObject(kernel, error, "phone compute specialization");
    sample[@"specialization_ns"] = @(elapsed(start));
    checkContinue(shouldContinue);

    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Uint;
    error = nil;
    start = Clock::now();
    id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    sample[@"render_pso_ns"] = @(elapsed(start));
    sample[@"render_pso_diagnostic"] = errorInfo(error);
    requireObject(render, error, "phone legacy render pipeline");
    checkContinue(shouldContinue);
    error = nil;
    start = Clock::now();
    id<MTLComputePipelineState> compute = [device newComputePipelineStateWithFunction:kernel error:&error];
    sample[@"compute_pso_ns"] = @(elapsed(start));
    sample[@"compute_pso_diagnostic"] = errorInfo(error);
    requireObject(compute, error, "phone legacy compute pipeline");
    checkContinue(shouldContinue);

    NSData* inputData = phoneWordData(input);
    id<MTLBuffer> inputBuffer = [device newBufferWithBytes:inputData.bytes length:inputData.length
                                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:sizeof(Output) options:MTLResourceStorageModeShared];
    MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
                                                                                              width:4 height:4 mipmapped:NO];
    textureDescriptor.storageMode = MTLStorageModeShared;
    textureDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
    requireObject(inputBuffer, nil, "phone input buffer");
    requireObject(output, nil, "phone output buffer");
    requireObject(texture, nil, "phone readback texture");
    memset(output.contents, 0xcd, output.length);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    requireObject(command, nil, "phone legacy command buffer");
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLRenderCommandEncoder> drawing = [command renderCommandEncoderWithDescriptor:pass];
    requireObject(drawing, nil, "phone legacy render encoder");
    [drawing setRenderPipelineState:render];
    [drawing setFragmentBuffer:inputBuffer offset:0 atIndex:0];
    [drawing setViewport:MTLViewport{0, 0, 4, 4, 0, 1}];
    [drawing drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [drawing endEncoding];
    id<MTLComputeCommandEncoder> computing = [command computeCommandEncoder];
    requireObject(computing, nil, "phone legacy compute encoder");
    [computing setComputePipelineState:compute];
    [computing setBuffer:inputBuffer offset:0 atIndex:0];
    [computing setBuffer:output offset:0 atIndex:1];
    [computing dispatchThreads:MTLSizeMake(LaneCount, 1, 1) threadsPerThreadgroup:MTLSizeMake(LaneCount, 1, 1)];
    [computing endEncoding];
    checkContinue(shouldContinue);
    [command commit];
    [command waitUntilCompleted]; // Retain and drain issued work; never claim cancellation.
    sample[@"command_status_value"] = @(command.status);
    sample[@"command_error"] = errorInfo(command.error);
    require(command.status == MTLCommandBufferStatusCompleted && !command.error, "phone GPU command failed");
    Output renderWords{}, computeWords{};
    [texture getBytes:renderWords.data() bytesPerRow:4 * 4 * sizeof(uint32_t)
           fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    memcpy(computeWords.data(), output.contents, sizeof(computeWords));
    sample[@"render_words"] = words(renderWords.data(), renderWords.size());
    sample[@"compute_words"] = words(computeWords.data(), computeWords.size());
    sample[@"command_status"] = @"completed";
    require(renderWords == expectedRender, "phone integer render readback differs from CPU oracle");
    require(computeWords == expectedCompute, "phone integer compute readback differs from CPU oracle");
    sample[@"oracle_verified"] = @YES;
}
#endif

NSDictionary* SkyrimMetalRunPhoneComparison(NSString* sourcePath, NSUInteger repeats,
                                            BOOL metal4First, BOOL (^shouldContinue)(void),
                                            void (^progress)(NSDictionary*)) {
    @autoreleasepool {
#if TARGET_OS_SIMULATOR
        (void)sourcePath; // The unsupported preview never reads shader inputs.
#endif
        auto started = Clock::now();
        NSMutableDictionary* report = [@{
            @"schema": @5, @"scope": @"authored-iphone-metal-workload-comparison",
            @"status": @"running", @"phase": @"prepare", @"repeat": @(repeats),
            @"metal4_first_requested": @(metal4First), @"completed_samples": @0,
            @"contention_panel_requested": metal4First ? @"alternate" : @"primary",
            @"contention_selector_scope": @"both APIs in each run; alternate selects a disjoint predeclared source panel and changes starting API; repeating a selector reuses its sources; OS/compiler cache is uncontrolled",
            @"total_samples": @0, @"total_cases": @(SkyrimPhoneWorkloads::CaseCount),
            @"total_families": @(SkyrimPhoneWorkloads::FamilyCount), @"variants_per_family": @(SkyrimPhoneWorkloads::VariantCount),
            @"control_source_options": sourceOptions(), @"workloads": [NSMutableArray new],
            @"progress_timing": phoneProgressTiming(),
            @"serial_comparison_verified": @NO, @"contention_control_verified": @NO,
            @"contention_control": @{ @"status": @"not_run", @"scope": @"separate authored MRC scheduling control" },
            @"samples": [NSMutableArray new], @"rejections": [NSMutableArray new],
            @"async_retention_checks": [NSMutableArray new],
            @"gpu_execution_verified": @NO, @"metal4_library_verified": @NO,
            @"metal4_async_adapter_verified": @NO, @"physical_iphone_verified": @NO,
            @"full_metal4_verified": @NO, @"full_moltenvk_verified": @NO,
            @"game_rendering_verified": @NO, @"performance_improvement_verified": @NO,
            @"production_async_lifetime_verified": @NO, @"cold_os_cache_verified": @NO,
            @"metal4_compiler_created": @NO, @"compiler_setup_ns": @0,
            @"cancellation_requested": @NO, @"stop_requested": @NO,
            @"timing_scope": @"wall intervals; async library includes callback and task join; PSOs use legacy APIs",
            @"specialization_scope": @"vertex lookup plus fragment and compute specialization",
            @"workload_scope": @"three authored integer families with four semantic variants each, runtime input buffers, serial execution; no game inputs",
            @"gpu_time_measured": @NO, @"parallel_compilation_verified": @NO,
            @"mrc_lifetime_verified": @NO,
            @"cache_scope": @"first observed sample and repeated samples; OS/compiler cache is not reset or controlled",
            @"stop_scope": @"cooperative boundaries only; issued compiler tasks and GPU work drain on worker without cancellation",
            @"environment_before": phoneEnvironment()
        } mutableCopy];
        report[@"progress_timing"][@"callback_present"] = @(progress != nil);
        @try {
            try {
                require(!NSThread.isMainThread, "phone comparison must run off the main thread");
                require(repeats == SkyrimPhoneWorkloads::RepeatCount, "phone repeat must be 4 for balanced route positions");
                report[@"total_samples"] = @(repeats * SkyrimPhoneWorkloads::RouteCount * SkyrimPhoneWorkloads::CaseCount);
                NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
                report[@"os_version"] = @[@(os.majorVersion), @(os.minorVersion), @(os.patchVersion)];
                struct utsname machine{};
                bool haveMachine = uname(&machine) == 0;
                NSString* model = haveMachine ? [NSString stringWithUTF8String:machine.machine] : nil;
                report[@"hardware_model"] = model ?: @"unknown";
#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR && !TARGET_OS_MACCATALYST
                BOOL physicalPhone = [model hasPrefix:@"iPhone"];
#else
                BOOL physicalPhone = NO;
#endif
                report[@"physical_iphone_eligible"] = @(physicalPhone);
                phoneCheckpoint(report, @"device", shouldContinue, progress);
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                recordDevices(report, device);
                if (!physicalPhone) {
                    report[@"status"] = @"unsupported";
                    report[@"unsupported_reason"] = @"physical_iphone_required";
                    throw std::runtime_error("this host comparison requires a physical iPhone");
                }
#if !TARGET_OS_SIMULATOR
                if (@available(macOS 26.0, iOS 26.0, *)) {
                    if (!device || ![report[@"device"][@"supports_metal4"] boolValue] ||
                        ![report[@"device"][@"new_compiler_selector_available"] boolValue]) {
                        report[@"status"] = @"unsupported";
                        report[@"unsupported_reason"] = !device ? @"no_default_metal_device" : @"metal4_unavailable";
                        throw std::runtime_error("a Metal 4 capable device and compiler factory are required");
                    }
                } else {
                    report[@"status"] = @"unsupported";
                    report[@"unsupported_reason"] = @"os_before_26";
                    throw std::runtime_error("iOS 26 or later is required");
                }
                phoneCheckpoint(report, @"source", shouldContinue, progress);
                NSError* error = nil;
                NSData* sourceBytes = [NSData dataWithContentsOfFile:sourcePath options:0 error:&error];
                report[@"input_diagnostic"] = errorInfo(error);
                requireObject(sourceBytes, error, "authored source read");
                NSString* sourceHash = phoneSourceHash(sourceBytes);
                report[@"source_sha256"] = sourceHash;
                report[@"source_bytes"] = @(sourceBytes.length);
                // This entry point runs only the bundled authored fixture, never
                // a game-derived or externally supplied shader.
                require([sourceHash isEqualToString:@"26ed56a8f4d264965eb624911d42cbf925f1329f0f103d58f683fe1c52d07ad6"],
                        "bundled authored fixture hash mismatch");
                NSString* source = [[NSString alloc] initWithData:sourceBytes encoding:NSUTF8StringEncoding];
                requireObject(source, nil, "authored UTF-8 source");
                checkContinue(shouldContinue);
                NSString* controlPath = [[sourcePath stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"fixture.metal"];
                error = nil;
                NSData* controlBytes = [NSData dataWithContentsOfFile:controlPath options:0 error:&error];
                report[@"control_input_diagnostic"] = errorInfo(error);
                requireObject(controlBytes, error, "authored control source read");
                NSString* controlHash = phoneSourceHash(controlBytes);
                require([controlHash isEqualToString:@"e45755523aabf64eb236c01a77ae220e271b4ae6d0eabcf120d9a80cabbada87"],
                        "bundled authored control fixture hash mismatch");
                NSString* controlSource = [[NSString alloc] initWithData:controlBytes encoding:NSUTF8StringEncoding];
                requireObject(controlSource, nil, "authored UTF-8 control source");
                report[@"control_source_sha256"] = controlHash;
                NSArray<NSString*>* routes = metal4First
                    ? @[@"metal4_source", @"legacy_source", @"metal4_async_source", @"legacy_async_source"]
                    : @[@"legacy_source", @"metal4_source", @"legacy_async_source", @"metal4_async_source"];
                NSMutableArray* workloads = report[@"workloads"];
                for (unsigned index = 0; index < SkyrimPhoneWorkloads::CaseCount; ++index)
                    [workloads addObject:phoneWorkloadInfo(index, sourceHash,
                        routes[SkyrimPhoneWorkloads::routeIndex(0, 0, index)])];
                phoneCheckpoint(report, @"compiler", shouldContinue, progress);
                auto compilerStarted = Clock::now();
                error = nil;
                id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
                report[@"compiler_setup_ns"] = @(elapsed(compilerStarted));
                report[@"compiler_diagnostic"] = errorInfo(error);
                report[@"metal4_compiler_created"] = @(compiler != nil);
                requireObject(compiler, error, "Metal 4 compiler creation");
                checkContinue(shouldContinue);
                id<MTLCommandQueue> queue = [device newCommandQueue];
                requireObject(queue, nil, "legacy command queue");
                report[@"base_route_order"] = routes;
                report[@"order_policy"] = @"leader=(family_index0+variant_index0)%4; each route is first once per family; four rotations per case balance all slots; initial backend selector permutes base order";
                NSMutableArray* samples = report[@"samples"];
                for (unsigned index = 0; index < SkyrimPhoneWorkloads::CaseCount; ++index) {
                    const auto input = SkyrimPhoneWorkloads::makeInput(index);
                    NSDictionary* workload = workloads[index];
                    report[@"active_case_id"] = workload[@"case_id"];
                    report[@"active_case_index"] = workload[@"case_index"];
                    report[@"active_family_id"] = workload[@"family_id"];
                    report[@"active_family_index"] = workload[@"family_index"];
                    report[@"active_variant_index"] = workload[@"variant_index"];
                    report[@"active_api"] = workload[@"case_first_api"];
                    phoneCheckpoint(report, @"case_start", shouldContinue, progress);
                    for (unsigned iteration = 0; iteration < repeats; ++iteration) {
                        for (unsigned slot = 0; slot < routes.count; ++slot) {
                            @autoreleasepool {
                                NSString* api = routes[SkyrimPhoneWorkloads::routeIndex(iteration, slot, index)];
                                bool metal4 = [api hasPrefix:@"metal4_"];
                                bool asynchronous = [api containsString:@"_async_"];
                                NSMutableDictionary* sample = [workload mutableCopy];
                                [sample addEntriesFromDictionary:@{
                                    @"api": api, @"iteration": @(iteration + 1), @"slot": @(slot + 1),
                                    @"sample_index": @(samples.count + 1), @"status": @"running",
                                    @"cache_observation": iteration ? @"repeated_case_api_in_this_run" : @"first_observed_case_api_in_this_run",
                                    @"library_operation": asynchronous ? @"async_compile" : @"compile",
                                    @"environment_before": phoneEnvironment()
                                }];
                                [samples addObject:sample];
                                report[@"active_api"] = api;
                                report[@"active_sample"] = sample[@"sample_index"];
                                phoneCheckpoint(report, @"library", shouldContinue, progress);
                                MTLCompileOptions* options = phoneCompileOptions(index);
                                auto libraryStarted = Clock::now();
                                error = nil;
                                id<MTLLibrary> library = asynchronous
                                    ? asyncSourceLibrary(device, compiler, source, metal4, &error, sample, options)
                                    : sourceLibrary(device, compiler, source, metal4, &error, options);
                                sample[@"library_ns"] = @(elapsed(libraryStarted));
                                sample[@"library_diagnostic"] = errorInfo(error);
                                requireObject(library, error, "phone library creation");
                                phoneCheckpoint(report, @"gpu_readback", shouldContinue, progress);
                                runPhoneWorkloadGPU(device, queue, library, index, input, sample, shouldContinue);
                                sample[@"environment_after"] = phoneEnvironment();
                                sample[@"status"] = @"passed";
                                report[@"completed_samples"] = @([report[@"completed_samples"] unsignedIntegerValue] + 1);
                                phoneCheckpoint(report, @"sample_complete", shouldContinue, progress);
                            }
                        }
                    }
                }
                report[@"active_case_id"] = @"control_fixture";
                report[@"active_case_index"] = @0;
                report[@"active_family_id"] = @"control_fixture";
                report[@"active_family_index"] = @0;
                report[@"active_variant_index"] = @0;
                NSString* invalid = [@"#error Intentional_Metal_compile_fixture_rejection\n" stringByAppendingString:controlSource];
                NSMutableArray* rejections = report[@"rejections"];
                for (NSString* api in routes) {
                    report[@"active_api"] = api;
                    phoneCheckpoint(report, @"invalid_source", shouldContinue, progress);
                    NSMutableDictionary* rejection = [@{ @"api": api } mutableCopy];
                    [rejections addObject:rejection];
                    auto rejectionStarted = Clock::now();
                    error = nil;
                    bool metal4 = [api hasPrefix:@"metal4_"];
                    id<MTLLibrary> library = [api containsString:@"_async_"]
                        ? asyncSourceLibrary(device, compiler, invalid, metal4, &error, rejection)
                        : sourceLibrary(device, compiler, invalid, metal4, &error);
                    [rejection addEntriesFromDictionary:@{
                        @"library_ns": @(elapsed(rejectionStarted)), @"returned_nil": @(library == nil),
                        @"error_present": @(error != nil), @"error": errorInfo(error)
                    }];
                    require(!library && error, "invalid source did not return nil library and NSError");
                }
                for (NSString* api in @[@"legacy_async_source", @"metal4_async_source"]) {
                    bool metal4 = [api hasPrefix:@"metal4_"];
                    report[@"active_api"] = api;
                    phoneCheckpoint(report, @"retained_success", shouldContinue, progress);
                    checkAsyncRetention(device, compiler, queue, controlSource, metal4, false,
                                        report[@"async_retention_checks"], shouldContinue);
                    phoneCheckpoint(report, @"retained_error", shouldContinue, progress);
                    checkAsyncRetention(device, compiler, queue, invalid, metal4, true,
                                        report[@"async_retention_checks"], shouldContinue);
                }
                checkContinue(shouldContinue);
                report[@"serial_comparison_verified"] = @YES;
                report[@"active_case_id"] = @"contention_control";
                report[@"active_case_index"] = @0;
                report[@"active_family_id"] = @"contention_control";
                report[@"active_family_index"] = @0;
                report[@"active_variant_index"] = @0;
                report[@"contention_control"] = @{
                    @"status": @"incomplete", @"scope": @"separate authored MRC scheduling control",
                    @"reason": @"entered control boundary; no completed result yet; submission or execution is not asserted"
                };
                phoneCheckpoint(report, @"contention_control_start", shouldContinue, progress);
                NSDictionary* contention = SkyrimMetalRunPhoneContention(sourcePath, metal4First, shouldContinue);
                if (![contention isKindOfClass:NSDictionary.class]) {
                    report[@"contention_control"] = @{
                        @"status": @"failed", @"scope": @"separate authored MRC scheduling control",
                        @"failure": @"control returned no dictionary result"
                    };
                    throw std::runtime_error("contention control returned no result");
                }
                report[@"contention_control"] = contention;
                phoneCheckpoint(report, @"contention_control_complete", shouldContinue, progress);
                NSString* contentionStatus = contention[@"status"];
                if ([contentionStatus isEqual:@"stopped"] || [contentionStatus isEqual:@"interrupted"])
                    throw ProbeInterrupted();
                if ([contentionStatus isEqual:@"unsupported"]) {
                    report[@"status"] = @"unsupported";
                    report[@"unsupported_reason"] = @"contention_control_unsupported";
                    throw std::runtime_error("separate contention control is unsupported; see nested result");
                }
                require([contentionStatus isEqual:@"passed"], "separate contention control failed; see nested result");
                require([contention[@"schema"] isEqual:@2], "contention control design does not match this comparison build");
                report[@"contention_control_verified"] = @YES;
                report[@"gpu_execution_verified"] = @YES;
                report[@"metal4_library_verified"] = @YES;
                report[@"metal4_async_adapter_verified"] = @YES;
                report[@"physical_iphone_verified"] = @YES;
                report[@"status"] = @"passed";
                report[@"phase"] = @"complete";
#endif
            } catch (const ProbeInterrupted& exception) {
                report[@"status"] = @"interrupted";
                report[@"stop_requested"] = @YES;
                report[@"failure"] = [NSString stringWithUTF8String:exception.what()];
            } catch (const std::exception& exception) {
                if (![report[@"status"] isEqual:@"unsupported"]) report[@"status"] = @"failed";
                report[@"failure"] = [NSString stringWithUTF8String:exception.what()] ?: @"native failure";
            } catch (...) {
                report[@"status"] = @"failed";
                report[@"failure"] = @"unknown native exception";
            }
        } @catch (NSException* exception) {
            report[@"status"] = @"failed";
            report[@"failure"] = @"Objective-C exception";
            report[@"exception_name"] = exception.name ?: @"unknown";
            report[@"exception_reason"] = exception.reason ?: @"";
        }
        report[@"environment_after"] = phoneEnvironment();
        report[@"elapsed_ns"] = @(elapsed(started));
        if (![report[@"status"] isEqual:@"passed"]) {
            NSMutableDictionary* unfinished = [report[@"samples"] lastObject];
            if ([unfinished[@"status"] isEqual:@"running"])
                unfinished[@"status"] = report[@"status"];
            // Partial sample success is retained, but cannot promote an
            // interrupted/unsupported/failed whole run to a verified claim.
            report[@"gpu_execution_verified"] = @NO;
            report[@"metal4_library_verified"] = @NO;
            report[@"metal4_async_adapter_verified"] = @NO;
            report[@"physical_iphone_verified"] = @NO;
        }
        return phoneSnapshot(report);
    }
}
#endif
