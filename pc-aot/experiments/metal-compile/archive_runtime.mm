// Authored legacy archive save/reload test; no Metal 4 API or hardware gate.
// https://developer.apple.com/documentation/metal/mtlpipelineoption/failonbinaryarchivemiss
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static void phase(const char* name) {
    std::printf("legacy_archive phase=%s\n", name);
    std::fflush(stdout);
}

static NSDictionary* errorInfo(NSError* error) {
    if (!error) return @{};
    return @{ @"domain": error.domain ?: @"", @"code": @(error.code),
              @"description": error.localizedDescription ?: @"" };
}

static bool writeReport(NSString* path, NSDictionary* report) {
    if (!path) return false;
    NSError* error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:report
        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&error];
    return data && [data writeToFile:path options:NSDataWritingAtomic error:&error];
}

static MTLComputePipelineDescriptor* descriptor(id<MTLLibrary> library, NSString* name,
    bool specialized, uint32_t value, NSMutableArray* specializations) {
    id<MTLFunction> function;
    if (specialized) {
        MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
        [constants setConstantValue:&value type:MTLDataTypeUInt atIndex:0];
        NSError* error = nil;
        function = [library newFunctionWithName:name constantValues:constants error:&error];
        [specializations addObject:@{ @"kernel": name, @"index": @0, @"type": @"uint", @"value": @(value),
            @"created": @(function != nil), @"error_present": @(error != nil), @"error": errorInfo(error) }];
        require(function && !error, "function constant specialization failed");
    } else {
        function = [library newFunctionWithName:name];
    }
    require(function != nil, "authored kernel is missing from the IR library");
    MTLComputePipelineDescriptor* result = [MTLComputePipelineDescriptor new];
    result.computeFunction = function;
    return result;
}

static void readback(id<MTLDevice> device, id<MTLComputePipelineState> pipeline,
                     bool recorded, bool specialized, NSMutableDictionary* observation) {
    phase(recorded ? "recorded_readback_setup" : "unrecorded_readback_setup");
    id<MTLBuffer> buffer = [device newBufferWithLength:16 * sizeof(uint32_t)
        options:MTLResourceStorageModeShared];
    require(buffer && buffer.contents, "shared readback buffer creation failed");
    std::memset(buffer.contents, 0xa5, buffer.length);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    require(queue && command && encoder, "compute command setup failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
    [encoder endEncoding];
    phase("compute_command_commit");
    [command commit];
    phase("compute_command_wait");
    [command waitUntilCompleted]; // The Python supervisor bounds the owned process.
    observation[@"command_status"] = command.status == MTLCommandBufferStatusCompleted ? @"completed" : @"failed";
    observation[@"command_error"] = errorInfo(command.error);
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
            "GPU command did not complete successfully");
    const uint32_t* words = static_cast<const uint32_t*>(buffer.contents);
    NSMutableArray* values = [NSMutableArray arrayWithCapacity:16];
    bool matches = true;
    for (uint32_t index = 0; index < 4; ++index) {
        uint32_t expected[4] = {
            recorded ? index + 3u : index * 5u + 11u,
            recorded ? (index + 1u) * 7u : (index ^ 3u) + 17u,
            recorded ? 10u : 29u - index,
            recorded ? 0x12345678u : 0x87654321u,
        };
        if (specialized) {
            uint32_t value = recorded ? 7u : 11u;
            expected[0] = index + value;
            expected[1] = (index + 1u) * value;
            expected[2] = value ^ 0x12345678u;
            expected[3] = 100u - value;
        }
        for (uint32_t component = 0; component < 4; ++component) {
            uint32_t actual = words[4 * index + component];
            [values addObject:@(actual)];
            matches = matches && actual == expected[component];
        }
    }
    observation[@"compute_words"] = values;
    require(matches, "exact authored compute readback mismatch");
}

static id<MTLComputePipelineState> createPipeline(id<MTLDevice> device,
    MTLComputePipelineDescriptor* configuration, bool strict, NSString* caseName,
    bool specialized, uint32_t value, NSMutableArray* observations) {
    NSMutableDictionary* item = [@{ @"case": caseName,
        @"kernel": configuration.computeFunction.name,
        @"strict": @(strict), @"archive_attached": @(configuration.binaryArchives.count != 0) } mutableCopy];
    item[@"function_constants"] = specialized ? @[@{ @"index": @0, @"type": @"uint", @"value": @(value) }] : @[];
    [observations addObject:item];
    NSError* error = nil;
    MTLPipelineOption options = strict ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
    std::printf("legacy_archive phase=pipeline_create case=%s\n", caseName.UTF8String);
    std::fflush(stdout);
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithDescriptor:configuration
        options:options reflection:nil error:&error];
    item[@"created"] = @(pipeline != nil);
    item[@"error_present"] = @(error != nil);
    item[@"error"] = errorInfo(error);
    return pipeline;
}

static void rejectArchive(id<MTLDevice> device, NSString* path, NSString* caseName,
                          NSMutableArray* observations) {
    MTLBinaryArchiveDescriptor* configuration = [MTLBinaryArchiveDescriptor new];
    configuration.url = [NSURL fileURLWithPath:path];
    NSError* error = nil;
    std::printf("legacy_archive phase=invalid_archive_factory case=%s\n", caseName.UTF8String);
    std::fflush(stdout);
    id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:configuration error:&error];
    [observations addObject:@{ @"case": caseName, @"returned_nil": @(archive == nil),
        @"error_present": @(error != nil), @"error": errorInfo(error) }];
    require(archive == nil && error != nil, "invalid archive was not rejected with an error");
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSString* reportPath = nil;
        NSMutableArray* pipelines = [NSMutableArray array];
        NSMutableArray* rejections = [NSMutableArray array];
        NSMutableArray* specializations = [NSMutableArray array];
        NSMutableDictionary* report = [@{
            @"schema": @1, @"scope": @"authored-legacy-metal-binary-archive",
            @"status": @"failed", @"process_id": @(getpid()),
            @"source_library_compiles": @0, @"url_library_loads": @0,
            @"gpu_execution_verified": @NO, @"metal4_verified": @NO,
            @"windows_archive_runtime_verified": @NO, @"nil_library_lookup_verified": @NO,
            @"physical_iphone_verified": @NO, @"game_rendering_verified": @NO,
            @"performance_improvement_verified": @NO, @"cold_os_cache_verified": @NO,
            @"production_compile_settings_parity_verified": @NO,
            @"harvested_kernels": @[], @"pipelines": pipelines, @"archive_rejections": rejections,
            @"function_specializations": specializations, @"variant_mode": @"basic",
        } mutableCopy];
        int exitCode = 1;
        @try {
            try {
                NSMutableDictionary* arguments = [NSMutableDictionary dictionary];
                for (int index = 1; index < argc; index += 2) {
                    require(index + 1 < argc, "arguments require a value");
                    NSString* key = [NSString stringWithUTF8String:argv[index]];
                    NSString* value = [NSString stringWithUTF8String:argv[index + 1]];
                    require(key && value && arguments[key] == nil, "invalid or repeated argument");
                    require([@[@"--mode", @"--library", @"--archive", @"--empty", @"--malformed", @"--report", @"--variant-mode"] containsObject:key],
                            "unknown argument");
                    arguments[key] = value;
                    if ([key isEqualToString:@"--report"]) reportPath = value;
                }
                NSString* mode = arguments[@"--mode"];
                bool harvesting = [mode isEqualToString:@"harvest"];
                require(harvesting || [mode isEqualToString:@"restore"], "mode must be harvest or restore");
                require(reportPath && arguments[@"--library"] && arguments[@"--archive"], "missing path argument");
                require(harvesting || (arguments[@"--empty"] && arguments[@"--malformed"]), "missing invalid archive inputs");
                report[@"mode"] = mode;
                NSString* variantMode = arguments[@"--variant-mode"] ?: @"basic";
                bool specialized = [variantMode isEqualToString:@"specialized"];
                require(specialized || [variantMode isEqualToString:@"basic"], "unknown variant mode");
                report[@"variant_mode"] = variantMode;
                if (specialized) report[@"scope"] = @"authored-legacy-metal-specialized-binary-archive";
                NSOperatingSystemVersion version = NSProcessInfo.processInfo.operatingSystemVersion;
                report[@"os_version"] = @[@(version.majorVersion), @(version.minorVersion), @(version.patchVersion)];
                require(!specialized || version.majorVersion >= 15, "specialized archive serialization requires macOS 15 or newer");
                phase("default_device");
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                report[@"device"] = @{ @"available": @(device != nil), @"name": device.name ?: @"" };
                if (!device) {
                    report[@"status"] = @"unsupported";
                    report[@"unsupported_reason"] = @"no_default_metal_device";
                    exitCode = 78;
                } else {
                    NSError* error = nil;
                    report[@"url_library_loads"] = @1;
                    phase("offline_ir_library_load");
                    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:arguments[@"--library"]]
                                                               error:&error];
                    report[@"library_load"] = @{ @"created": @(library != nil), @"error_present": @(error != nil),
                        @"error": errorInfo(error) };
                    require(library && !error, "offline IR library load failed");
                    report[@"library_function_names"] = [library.functionNames sortedArrayUsingSelector:@selector(compare:)] ?: @[];
                    MTLBinaryArchiveDescriptor* archiveDescriptor = [MTLBinaryArchiveDescriptor new];
                    NSURL* archiveURL = [NSURL fileURLWithPath:arguments[@"--archive"]];
                    if (harvesting) {
                        require(![NSFileManager.defaultManager fileExistsAtPath:archiveURL.path], "refusing to overwrite archive");
                    } else {
                        archiveDescriptor.url = archiveURL;
                    }
                    error = nil;
                    phase(harvesting ? "fresh_archive_factory" : "saved_archive_load");
                    id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:archiveDescriptor error:&error];
                    report[@"archive_factory"] = @{ @"kind": harvesting ? @"fresh" : @"saved",
                        @"created": @(archive != nil), @"error_present": @(error != nil), @"error": errorInfo(error) };
                    require(archive && !error, "binary archive creation/load failed");
                    NSString* recordedName = specialized ? @"archive_specialized" : @"archive_recorded";
                    MTLComputePipelineDescriptor* recorded = descriptor(library, recordedName, specialized, 7, specializations);
                    if (harvesting) {
                        error = nil;
                        phase("archive_add_recorded_kernel");
                        BOOL added = [archive addComputePipelineFunctionsWithDescriptor:recorded error:&error];
                        report[@"archive_add"] = @{ @"succeeded": @(added), @"error_present": @(error != nil), @"error": errorInfo(error) };
                        require(added && !error, "adding the recorded kernel failed");
                        report[@"harvested_kernels"] = @[recordedName];
                    }
                    recorded.binaryArchives = @[archive];
                    id<MTLComputePipelineState> hit = createPipeline(device, recorded, true,
                        harvesting ? @"harvest_recorded_strict" : @"restore_recorded_strict", specialized, 7, pipelines);
                    require(hit && ![pipelines.lastObject[@"error_present"] boolValue], "recorded strict archive lookup failed");
                    readback(device, hit, true, specialized, pipelines.lastObject);
                    if (harvesting) {
                        error = nil;
                        phase("archive_serialize");
                        BOOL saved = [archive serializeToURL:archiveURL error:&error];
                        report[@"archive_serialize"] = @{ @"succeeded": @(saved), @"error_present": @(error != nil), @"error": errorInfo(error) };
                        require(saved && !error, "binary archive serialization failed");
                    } else {
                        NSString* missingName = specialized ? @"archive_specialized" : @"archive_unrecorded";
                        MTLComputePipelineDescriptor* missing = descriptor(library, missingName, specialized, 11, specializations);
                        missing.binaryArchives = @[archive];
                        id<MTLComputePipelineState> miss = createPipeline(device, missing, true, @"restore_unrecorded_strict", specialized, 11, pipelines);
                        require(!miss && [pipelines.lastObject[@"error_present"] boolValue], "unrecorded strict lookup did not fail with an error");
                        // The fallback is explicit and cannot modify the saved archive.
                        missing.binaryArchives = @[];
                        id<MTLComputePipelineState> fallback = createPipeline(device, missing, false, @"restore_unrecorded_fallback", specialized, 11, pipelines);
                        require(fallback && ![pipelines.lastObject[@"error_present"] boolValue], "explicit normal fallback failed");
                        readback(device, fallback, false, specialized, pipelines.lastObject);
                        rejectArchive(device, arguments[@"--empty"], @"empty_file", rejections);
                        rejectArchive(device, arguments[@"--malformed"], @"malformed_file", rejections);
                    }
                    report[@"gpu_execution_verified"] = @YES;
                    report[@"status"] = @"passed";
                    exitCode = 0;
                }
            } catch (const std::exception& error) {
                report[@"failure"] = [NSString stringWithUTF8String:error.what()] ?: @"C++ exception";
            }
        } @catch (NSException* error) {
            report[@"exception"] = @{ @"name": error.name ?: @"", @"reason": error.reason ?: @"" };
        }
        if (!writeReport(reportPath, report)) {
            std::fprintf(stderr, "failed to write native archive evidence\n");
            return 1;
        }
        NSString* reportedMode = report[@"mode"] ?: @"invalid";
        std::printf("legacy_archive mode=%s status=%s\n", reportedMode.UTF8String, [report[@"status"] UTF8String]);
        return exitCode;
    }
}
