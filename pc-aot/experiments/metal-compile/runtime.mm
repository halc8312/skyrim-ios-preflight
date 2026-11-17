// Standalone authored-library experiment. This is not a MoltenVK replacement.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>

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

#include <array>
#include <chrono>
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

// Record capability names and booleans before any eligibility guard. This
// distinguishes a missing/virtualized device from a device lacking a family.
// Do not include registry IDs or substitute a different device for the default.
static NSDictionary* deviceInfo(id<MTLDevice> device) {
    NSMutableDictionary* info = [@{
        @"available": @(device != nil), @"name": device.name ?: @"",
        @"supports_metal4": @NO, @"metal4_family_queried": @NO,
        @"supports_metal3": @NO, @"supports_mac2": @NO,
        @"supports_apple7": @NO, @"supports_apple8": @NO, @"supports_apple9": @NO,
        @"new_compiler_selector_available": @(device && [device respondsToSelector:@selector(newCompilerWithDescriptor:error:)])
    } mutableCopy];
    if (!device) return info;
    info[@"supports_metal3"] = @([device supportsFamily:MTLGPUFamilyMetal3]);
    info[@"supports_mac2"] = @([device supportsFamily:MTLGPUFamilyMac2]);
    info[@"supports_apple7"] = @([device supportsFamily:MTLGPUFamilyApple7]);
    info[@"supports_apple8"] = @([device supportsFamily:MTLGPUFamilyApple8]);
    info[@"supports_apple9"] = @([device supportsFamily:MTLGPUFamilyApple9]);
    if (@available(macOS 26.0, iOS 26.0, *)) {
        info[@"metal4_family_queried"] = @YES;
        info[@"supports_metal4"] = @([device supportsFamily:MTLGPUFamilyMetal4]);
    }
    return info;
}

static void recordDevices(NSMutableDictionary* report, id<MTLDevice> device) {
    report[@"device"] = deviceInfo(device);
    report[@"default_device_available"] = @(device != nil);
    NSMutableArray* available = [NSMutableArray new];
    report[@"device_enumeration"] = @"all_devices";
    for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
        [available addObject:deviceInfo(candidate)];
    }
    report[@"available_devices"] = available;
    report[@"available_device_count"] = @(available.count);
}

static NSDictionary* errorInfo(NSError* error) {
    return error ? @{ @"domain": error.domain, @"code": @(error.code),
                     @"description": error.localizedDescription ?: @"" } : @{};
}

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

static id<MTLLibrary> sourceLibrary(id<MTLDevice> device, id<MTL4Compiler> compiler,
                                   NSString* source, bool metal4, NSError** error) {
    if (!metal4) return [device newLibraryWithSource:source options:compileOptions() error:error];
    MTL4LibraryDescriptor* descriptor = [MTL4LibraryDescriptor new];
    descriptor.source = source;
    descriptor.options = compileOptions();
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
                                        NSMutableDictionary* observation) {
    auto request = beginAsyncLibrary(device, compiler, source, compileOptions(), metal4, false);
    request.drain();
    return request.result(error, observation);
}

static NSArray* words(const uint32_t* data, size_t count) {
    NSMutableArray* values = [NSMutableArray arrayWithCapacity:count];
    for (size_t i = 0; i < count; ++i) [values addObject:@(data[i])];
    return values;
}

static void runGPU(id<MTLDevice> device, id<MTLCommandQueue> queue,
                   id<MTLLibrary> library, NSMutableDictionary* sample) {
    NSError* error = nil;
    auto start = Clock::now();
    id<MTLFunction> vertex = [library newFunctionWithName:@"probe_vertex"];
    requireObject(vertex, nil, "vertex function lookup");
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    uint32_t scale = 7;
    [constants setConstantValue:&scale type:MTLDataTypeUInt atIndex:0];
    id<MTLFunction> fragment = [library newFunctionWithName:@"probe_fragment"
                                          constantValues:constants error:&error];
    requireObject(fragment, error, "fragment specialization");
    error = nil;
    id<MTLFunction> kernel = [library newFunctionWithName:@"probe_kernel"
                                        constantValues:constants error:&error];
    requireObject(kernel, error, "compute specialization");
    sample[@"specialization_ns"] = @(elapsed(start));

    MTLRenderPipelineDescriptor* renderDescriptor = [MTLRenderPipelineDescriptor new];
    renderDescriptor.vertexFunction = vertex;
    renderDescriptor.fragmentFunction = fragment;
    renderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    error = nil;
    start = Clock::now();
    id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:renderDescriptor error:&error];
    sample[@"render_pso_ns"] = @(elapsed(start));
    requireObject(render, error, "legacy render pipeline");
    error = nil;
    start = Clock::now();
    id<MTLComputePipelineState> compute = [device newComputePipelineStateWithFunction:kernel error:&error];
    sample[@"compute_pso_ns"] = @(elapsed(start));
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
    [command commit];
    // The Python supervisor has a deadline. This API itself has no timeout.
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
                                bool metal4, bool invalid, NSMutableArray* observations) {
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
    NSError* error = nil;
    id<MTLLibrary> library = request.result(&error, observation);
    observation[@"returned_nil"] = @(library == nil);
    observation[@"error_present"] = @(error != nil);
    observation[@"error"] = errorInfo(error);
    if (invalid) {
        require(!library && error, "late async rejection lost nil library or NSError");
    } else {
        requireObject(library, error, "late async library");
        runGPU(device, queue, library, observation);
    }
}

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
