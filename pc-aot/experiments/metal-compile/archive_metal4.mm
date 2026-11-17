// Authored direct Metal 4 archive lookup; each case runs in a fresh process.
// No source compiler, compiler factory, or fallback pipeline creation is used.
// https://developer.apple.com/documentation/metal/mtl4archive
// https://developer.apple.com/documentation/metal/mtl4libraryfunctiondescriptor/library
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

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static void phase(const char* name) {
    std::printf("metal4_archive phase=%s\n", name);
    std::fflush(stdout);
}

static NSDictionary* errorInfo(NSError* error) {
    return error ? @{ @"present": @YES, @"domain": error.domain ?: @"",
        @"code": @(error.code), @"description": error.localizedDescription ?: @"" }
        : @{ @"present": @NO };
}

static bool writeReport(NSString* path, NSDictionary* report) {
    NSError* error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:report
        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&error];
    if (data && [data writeToFile:path options:NSDataWritingAtomic error:&error]) return true;
    std::fprintf(stderr, "failed to write native Metal 4 archive evidence: %s\n",
        (error.localizedDescription ?: @"unknown error").UTF8String);
    return false;
}

static NSMutableDictionary* deviceInfo(id<MTLDevice> device) {
    NSMutableDictionary* result = [@{
        @"available": @(device != nil), @"name": device.name ?: @"",
        @"architecture_query_status": device ? @"unavailable_os" : @"no_device",
        @"architecture_name": NSNull.null,
        @"supports_metal4": @NO, @"supports_apple9": @NO, @"supports_apple10": @NO,
        @"metal4_family_queried": @NO, @"apple9_family_queried": @NO,
        @"apple10_family_queried": @NO,
        @"archive_selector_available": @(device && [device respondsToSelector:@selector(newArchiveWithURL:error:)])
    } mutableCopy];
    if (!device) return result;
    if (@available(macOS 14.0, iOS 17.0, *)) {
        @try {
            NSString* name = device.architecture.name;
            result[@"architecture_query_status"] = name.length ? @"observed" : @"empty_result";
            result[@"architecture_name"] = name.length ? (id)name : NSNull.null;
        } @catch (NSException* error) {
            result[@"architecture_query_status"] = @"exception";
            result[@"architecture_query_exception"] = error.name ?: @"unknown";
        }
    }
    if (@available(macOS 14.0, iOS 17.0, *)) {
        result[@"apple9_family_queried"] = @YES;
        result[@"supports_apple9"] = @([device supportsFamily:MTLGPUFamilyApple9]);
    }
    if (@available(macOS 26.0, iOS 26.0, *)) {
        result[@"metal4_family_queried"] = @YES;
        result[@"apple10_family_queried"] = @YES;
        result[@"supports_metal4"] = @([device supportsFamily:MTLGPUFamilyMetal4]);
        result[@"supports_apple10"] = @([device supportsFamily:MTLGPUFamilyApple10]);
    }
    return result;
}

static NSArray* expectedWords(bool recorded) {
    NSMutableArray* result = [NSMutableArray arrayWithCapacity:16];
    for (uint32_t index = 0; index < 4; ++index) {
        const uint32_t words[4] = {
            recorded ? index + 3u : index * 5u + 11u,
            recorded ? (index + 1u) * 7u : (index ^ 3u) + 17u,
            recorded ? 10u : 29u - index,
            recorded ? 0x12345678u : 0x87654321u,
        };
        for (uint32_t word : words) [result addObject:@(word)];
    }
    return result;
}

static void readback(id<MTLDevice> device, id<MTLComputePipelineState> pipeline,
                     NSMutableDictionary* observation) {
    observation[@"attempted"] = @YES;
    phase("legacy_compute_readback_setup");
    id<MTLBuffer> buffer = [device newBufferWithLength:16 * sizeof(uint32_t)
        options:MTLResourceStorageModeShared];
    require(buffer && buffer.contents, "shared readback buffer creation failed");
    std::memset(buffer.contents, 0xa5, buffer.length);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    require(queue && command && encoder, "legacy compute command setup failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
    [encoder endEncoding];
    phase("legacy_compute_command_commit");
    [command commit];
    phase("legacy_compute_command_wait");
    [command waitUntilCompleted]; // The Python supervisor bounds the owned process.
    observation[@"command_status"] = command.status == MTLCommandBufferStatusCompleted ? @"completed" : @"failed";
    observation[@"command_status_value"] = @(command.status);
    observation[@"error"] = errorInfo(command.error);
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
        "legacy GPU command did not complete successfully");
    const uint32_t* words = static_cast<const uint32_t*>(buffer.contents);
    NSMutableArray* actual = [NSMutableArray arrayWithCapacity:16];
    for (size_t index = 0; index < 16; ++index) [actual addObject:@(words[index])];
    observation[@"words_u32"] = actual;
    const bool matches = [actual isEqualToArray:observation[@"expected_u32"]];
    observation[@"readback_matches"] = @(matches);
    require(matches, "exact authored compute readback mismatch");
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSString* reportPath = nil;
        bool reportPathApproved = false;
        NSMutableDictionary* counters = [@{ @"library_load_attempts": @0,
            @"archive_load_attempts": @0, @"direct_archive_lookup_attempts": @0 } mutableCopy];
        NSMutableDictionary* libraryResult = [@{ @"attempted": @NO, @"created": @NO,
            @"function_names": @[], @"error": errorInfo(nil) } mutableCopy];
        NSMutableDictionary* archiveResult = [@{ @"attempted": @NO, @"created": @NO,
            @"error": errorInfo(nil) } mutableCopy];
        NSMutableDictionary* lookup = [@{ @"attempted": @NO, @"pipeline_created": @NO,
            @"error": errorInfo(nil) } mutableCopy];
        NSMutableDictionary* gpu = [@{ @"attempted": @NO, @"command_status": @"not_submitted",
            @"command_status_value": NSNull.null, @"error": errorInfo(nil), @"words_u32": @[],
            @"expected_u32": @[], @"readback_matches": @NO } mutableCopy];
        NSMutableDictionary* report = [@{
            @"schema": @1, @"scope": @"authored-metal4-direct-archive-lookup",
            @"case": @"", @"kernel": @"", @"library_mode": @"none", @"status": @"failed",
            @"process_id": @(getpid()), @"source_library_compiles": @0, @"compiler_factory_calls": @0,
            @"counters": counters, @"library": libraryResult, @"archive": archiveResult,
            @"lookup": lookup, @"gpu": gpu, @"device": deviceInfo(nil),
            @"physical_iphone_verified": @NO, @"full_moltenvk_verified": @NO,
            @"game_rendering_verified": @NO, @"production_compile_options_parity_verified": @NO,
            @"performance_improvement_verified": @NO, @"zero_internal_compilation_verified": @NO
        } mutableCopy];
        NSOperatingSystemVersion version = NSProcessInfo.processInfo.operatingSystemVersion;
        report[@"os_version"] = @[@(version.majorVersion), @(version.minorVersion), @(version.patchVersion)];
        int exitCode = 1;
        @try {
            try {
                NSMutableDictionary* arguments = [NSMutableDictionary dictionary];
                for (int index = 1; index < argc; index += 2) {
                    require(index + 1 < argc, "arguments require a value");
                    NSString* key = [NSString stringWithUTF8String:argv[index]];
                    NSString* value = [NSString stringWithUTF8String:argv[index + 1]];
                    require(key && value.length && arguments[key] == nil, "invalid or repeated argument");
                    require([@[@"--case", @"--archive", @"--library", @"--report"] containsObject:key], "unknown argument");
                    arguments[key] = value;
                    if ([key isEqualToString:@"--report"]) reportPath = value;
                }
                require(reportPath != nil, "missing report path");
                // lstat also rejects an existing dangling symlink. The orchestrator
                // supplies a fresh, exclusively owned output directory.
                struct stat reportStat;
                errno = 0;
                require(lstat(reportPath.fileSystemRepresentation, &reportStat) != 0 && errno == ENOENT,
                    "refusing to overwrite an existing or inaccessible report path");
                reportPathApproved = true;
                NSString* caseName = arguments[@"--case"];
                require([@[@"probe-device", @"recorded-supplied", @"recorded-nil",
                    @"unrecorded-supplied", @"unrecorded-nil"] containsObject:caseName], "unknown or missing case");
                report[@"case"] = caseName;
                const bool probe = [caseName isEqualToString:@"probe-device"];
                const bool supplied = [caseName hasSuffix:@"-supplied"];
                const bool recorded = [caseName hasPrefix:@"recorded-"];
                if (probe) {
                    require(arguments[@"--archive"] == nil && arguments[@"--library"] == nil,
                        "probe-device accepts neither archive nor library");
                } else {
                    require(arguments[@"--archive"] != nil, "missing archive path");
                    require(supplied ? arguments[@"--library"] != nil : arguments[@"--library"] == nil,
                        "supplied cases require a library; nil cases reject any library argument");
                    report[@"kernel"] = recorded ? @"archive_recorded" : @"archive_unrecorded";
                    report[@"library_mode"] = supplied ? @"supplied" : @"nil";
                    gpu[@"expected_u32"] = expectedWords(recorded);
                }
                phase("default_device");
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                NSMutableDictionary* capabilities = deviceInfo(device);
                report[@"device"] = capabilities;
                if (@available(macOS 26.0, iOS 26.0, *)) {
                    NSString* unsupported = nil;
                    if (!device) unsupported = @"no_default_metal_device";
                    else if (![capabilities[@"supports_metal4"] boolValue]) unsupported = @"metal4_family_unavailable";
                    else if (![capabilities[@"archive_selector_available"] boolValue]) unsupported = @"archive_selector_unavailable";
                    else if (![capabilities[@"architecture_query_status"] isEqual:@"observed"]) unsupported = @"device_architecture_unavailable";
                    if (unsupported) {
                        report[@"status"] = @"unsupported";
                        report[@"unsupported_reason"] = unsupported;
                        exitCode = 78;
                    } else if (probe) {
                        report[@"status"] = @"device_supported";
                        exitCode = 0;
                    } else {
                        // Library loading is confined to this supplied-only branch.
                        id<MTLLibrary> library = nil;
                        NSError* error = nil;
                        if (supplied) {
                            counters[@"library_load_attempts"] = @1;
                            libraryResult[@"attempted"] = @YES;
                            phase("offline_ir_library_load");
                            library = [device newLibraryWithURL:[NSURL fileURLWithPath:arguments[@"--library"]] error:&error];
                            libraryResult[@"created"] = @(library != nil);
                            libraryResult[@"error"] = errorInfo(error);
                            require(library != nil, "offline authored IR library load failed");
                            NSArray* names = [library.functionNames sortedArrayUsingSelector:@selector(compare:)] ?: @[];
                            libraryResult[@"function_names"] = names;
                            require([names containsObject:@"archive_recorded"] && [names containsObject:@"archive_unrecorded"],
                                "authored IR library must contain both recorded and unrecorded controls");
                        }
                        error = nil;
                        counters[@"archive_load_attempts"] = @1;
                        archiveResult[@"attempted"] = @YES;
                        phase("metal4_saved_archive_load");
                        id<MTL4Archive> archive = [device newArchiveWithURL:[NSURL fileURLWithPath:arguments[@"--archive"]] error:&error];
                        archiveResult[@"created"] = @(archive != nil);
                        archiveResult[@"error"] = errorInfo(error);
                        require(archive != nil, "Metal 4 archive load failed");
                        MTL4LibraryFunctionDescriptor* function = [MTL4LibraryFunctionDescriptor new];
                        function.name = report[@"kernel"];
                        function.library = library; // Intentionally nil in the two omitted-library cases.
                        MTL4ComputePipelineDescriptor* descriptor = [MTL4ComputePipelineDescriptor new];
                        descriptor.computeFunctionDescriptor = function;
                        // Other fields use SDK defaults; the authored script also
                        // omits them. A miss does not prove that those defaults or
                        // nil-library identity are its cause.
                        error = nil;
                        counters[@"direct_archive_lookup_attempts"] = @1;
                        lookup[@"attempted"] = @YES;
                        phase("metal4_direct_archive_lookup");
                        id<MTLComputePipelineState> pipeline = [archive newComputePipelineStateWithDescriptor:descriptor error:&error];
                        lookup[@"pipeline_created"] = @(pipeline != nil);
                        lookup[@"error"] = errorInfo(error);
                        if (!pipeline) {
                            require(error != nil, "direct archive lookup returned nil without NSError");
                            if (recorded) {
                                require(!supplied, "recorded supplied-library direct archive lookup failed");
                                report[@"status"] = @"boundary_rejected";
                                exitCode = 3;
                            } else {
                                report[@"status"] = @"unrecorded_rejected";
                                exitCode = 0;
                            }
                        } else {
                            require(recorded, "unrecorded function unexpectedly produced a direct archive pipeline");
                            // A nonnil pipeline accompanied by an NSError warning is
                            // preserved and still requires the exact GPU readback.
                            readback(device, pipeline, gpu);
                            report[@"status"] = @"recorded_readback_matched";
                            exitCode = 0;
                        }
                    }
                } else {
                    report[@"status"] = @"unsupported";
                    report[@"unsupported_reason"] = @"metal4_requires_os_26";
                    exitCode = 78;
                }
            } catch (const std::exception& error) {
                report[@"status"] = @"failed";
                report[@"failure"] = [NSString stringWithUTF8String:error.what()] ?: @"C++ exception";
                exitCode = 1;
            } catch (...) {
                report[@"status"] = @"failed";
                report[@"failure"] = @"unknown C++ exception";
                exitCode = 1;
            }
        } @catch (NSException* error) {
            report[@"status"] = @"failed";
            report[@"exception"] = @{ @"name": error.name ?: @"", @"reason": error.reason ?: @"" };
            exitCode = 1;
        }
        if (!reportPathApproved || !writeReport(reportPath, report)) {
            if (!reportPathApproved) std::fprintf(stderr, "no fresh native report path was approved\n");
            return 1;
        }
        std::printf("metal4_archive case=%s status=%s\n", [report[@"case"] UTF8String], [report[@"status"] UTF8String]);
        return exitCode;
    }
}
