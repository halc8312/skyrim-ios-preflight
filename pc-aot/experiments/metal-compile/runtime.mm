// Standalone authored-library experiment. This is not a MoltenVK replacement.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#if !defined(__MAC_OS_X_VERSION_MAX_ALLOWED) || __MAC_OS_X_VERSION_MAX_ALLOWED < 260000
#error This fixture requires the macOS 26 SDK or later
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

using Clock = std::chrono::steady_clock;

static uint64_t elapsed(Clock::time_point start) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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

static id<MTLLibrary> sourceLibrary(id<MTLDevice> device, id<MTL4Compiler> compiler,
                                   NSString* source, bool metal4, NSError** error) {
    if (!metal4) return [device newLibraryWithSource:source options:compileOptions() error:error];
    MTL4LibraryDescriptor* descriptor = [MTL4LibraryDescriptor new];
    descriptor.source = source;
    descriptor.options = compileOptions();
    return [compiler newLibraryWithDescriptor:descriptor error:error];
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
        NSMutableDictionary* report = [@{
            @"schema": @1, @"status": @"failed", @"scope": @"authored-metal-library-api-and-readback",
            @"source_options": sourceOptions(), @"gpu_execution_verified": @NO,
            @"full_moltenvk_verified": @NO, @"physical_iphone_verified": @NO,
            @"game_rendering_verified": @NO, @"performance_improvement_verified": @NO,
            @"cold_os_cache_verified": @NO,
            @"specialization_scope": @"vertex lookup plus fragment and compute specialization",
            @"offline_compile_options_verified": @NO,
            @"samples": [NSMutableArray new], @"rejections": [NSMutableArray new]
        } mutableCopy];
        int result = 1;
        try {
            for (int i = 1; i < argc; i += 2) {
                require(i + 1 < argc, "missing command argument");
                const std::string key(argv[i]);
                NSString* value = [NSString stringWithUTF8String:argv[i + 1]];
                require(value != nil, "non-UTF8 argument");
                if (key == "--source" && !sourcePath) sourcePath = value;
                else if (key == "--report" && !reportPath) reportPath = value;
                else if (key == "--library" && !libraryPath) libraryPath = value;
                else if (key == "--repeat" && !repeats) {
                    require(strlen(argv[i + 1]) == 1 && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '5',
                            "repeat must be between 1 and 5");
                    repeats = unsigned(argv[i + 1][0] - '0');
                } else throw std::runtime_error("unknown or duplicate argument");
            }
            require(sourcePath && reportPath && repeats, "source, report and repeat are required");
            report[@"repeat"] = @(repeats);
            report[@"offline_library_requested"] = @(libraryPath != nil);
            NSOperatingSystemVersion version = NSProcessInfo.processInfo.operatingSystemVersion;
            report[@"os_version"] = @[@(version.majorVersion), @(version.minorVersion), @(version.patchVersion)];
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (@available(macOS 26.0, *)) {
                if (!device || ![device supportsFamily:MTLGPUFamilyMetal4]) {
                    report[@"status"] = @"unsupported";
                    throw std::runtime_error("a Metal 4 capable device is required");
                }
            } else {
                report[@"status"] = @"unsupported";
                throw std::runtime_error("macOS 26 or later is required");
            }
            report[@"device"] = @{ @"name": device.name, @"supports_metal4": @YES };
            NSError* error = nil;
            NSString* source = [NSString stringWithContentsOfFile:sourcePath encoding:NSUTF8StringEncoding error:&error];
            requireObject(source, error, "authored source read");
            auto start = Clock::now();
            error = nil;
            id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
            report[@"compiler_setup_ns"] = @(elapsed(start));
            requireObject(compiler, error, "Metal 4 compiler creation");
            id<MTLCommandQueue> queue = [device newCommandQueue];
            requireObject(queue, nil, "legacy command queue");
            NSMutableArray* samples = report[@"samples"];
            for (unsigned iteration = 0; iteration < repeats; ++iteration) {
                // Alternate the source routes. OS/compiler caches remain uncontrolled.
                for (unsigned slot = 0; slot < (libraryPath ? 3u : 2u); ++slot) {
                    @autoreleasepool {
                        bool offline = slot == 2;
                        bool metal4 = !offline && ((slot + iteration) % 2 != 0);
                        NSString* api = offline ? @"offline_metallib" : metal4 ? @"metal4_source" : @"legacy_source";
                        NSMutableDictionary* sample = [@{ @"api": api, @"iteration": @(iteration + 1),
                                                          @"library_operation": offline ? @"load" : @"compile" } mutableCopy];
                        [samples addObject:sample];
                        printf("iteration=%u api=%s library begin\n", iteration + 1, api.UTF8String);
                        fflush(stdout);
                        error = nil;
                        start = Clock::now();
                        id<MTLLibrary> library = offline
                            ? [device newLibraryWithURL:[NSURL fileURLWithPath:libraryPath] error:&error]
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
            for (unsigned mode = 0; mode < 2; ++mode) {
                error = nil;
                start = Clock::now();
                id<MTLLibrary> invalid = sourceLibrary(device, compiler, invalidSource, mode != 0, &error);
                [rejections addObject:@{ @"api": mode ? @"metal4_source" : @"legacy_source",
                                        @"library_ns": @(elapsed(start)), @"returned_nil": @(invalid == nil),
                                        @"error_present": @(error != nil), @"error": errorInfo(error) }];
                require(!invalid && error, "invalid source did not produce nil library plus NSError");
            }
            report[@"gpu_execution_verified"] = @YES;
            report[@"status"] = @"passed";
            result = 0;
        } catch (const std::exception& exception) {
            report[@"failure"] = [NSString stringWithUTF8String:exception.what()] ?: @"native failure";
            fprintf(stderr, "%s\n", exception.what());
            result = [report[@"status"] isEqual:@"unsupported"] ? 78 : 1;
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
