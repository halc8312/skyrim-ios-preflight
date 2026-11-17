// Authored integer expressions on legacy Metal. No Metal 4 API or game input.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "options_snapshot.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>

#if !defined(__MAC_OS_X_VERSION_MAX_ALLOWED) || __MAC_OS_X_VERSION_MAX_ALLOWED < 150000
#error This probe requires macOS SDK 15 or later
#endif

static void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

static void phase(NSString* name, const char* value) {
    std::printf("macro_integer case=%s phase=%s\n", name.UTF8String, value);
    std::fflush(stdout);
}

static void writeReport(NSDictionary* report, NSString* path, bool first = false) {
    NSError* error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
    require(data != nil, "report serialization failed");
    require([data writeToFile:path options:first ? NSDataWritingWithoutOverwriting : NSDataWritingAtomic error:&error],
            "report write failed");
}

static NSString* readSource(NSString* path) {
    std::ifstream input(path.fileSystemRepresentation, std::ios::binary);
    require(input.is_open(), "authored source cannot be opened");
    char buffer[16385];
    input.read(buffer, sizeof(buffer));
    const auto count = input.gcount();
    require(count > 0 && count <= 16384 && !input.bad(), "authored source size/read invalid");
    NSString* text = [[NSString alloc] initWithBytes:buffer length:size_t(count) encoding:NSUTF8StringEncoding];
    require(text != nil, "authored source is not UTF-8");
    return text;
}

static NSDictionary* errorInfo(NSError* error, NSString* name, const char* api) {
    if (!error) return @{ @"present": @NO };
    NSString* description = error.localizedDescription ?: @"";
    // The runner redirects stderr to a raw evidence file from process creation.
    // Preserve the entire description there even when the JSON preview is bounded.
    std::fprintf(stderr, "macro_integer case=%s api=%s domain=%s code=%lld\n%s\n",
        name.UTF8String, api, error.domain.UTF8String ?: "", (long long)error.code, description.UTF8String ?: "");
    std::fflush(stderr);
    NSData* data = [description dataUsingEncoding:NSUTF8StringEncoding];
    NSMutableDictionary* result = [@{ @"present": @YES, @"domain": error.domain ?: @"", @"code": @(error.code),
        @"diagnostic_bytes": @(data.length), @"diagnostics_truncated": @(data.length > 65536),
        @"full_diagnostics_in_native_stderr": @YES } mutableCopy];
    if (data.length > 65536)
        result[@"diagnostic_prefix_base64"] = [[data subdataWithRange:NSMakeRange(0, 65536)] base64EncodedStringWithOptions:0];
    else result[@"diagnostics"] = description;
    return result;
}

static MTLCompileOptions* authoredOptions(id value) {
    if (@available(macOS 15.0, *)) {
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        options.preserveInvariance = NO;
        options.optimizationLevel = MTLLibraryOptimizationLevelDefault;
        options.preprocessorMacros = @{ @"U32_MAX": value };
        require(options.languageVersion == MTLLanguageVersion3_1 && options.mathMode == MTLMathModeSafe
            && options.mathFloatingPointFunctions == MTLMathFloatingPointFunctionsPrecise
            && !options.preserveInvariance && options.optimizationLevel == MTLLibraryOptimizationLevelDefault,
            "authored option getters differ from requested profile");
        return options;
    }
    return nil;
}

static void runGPU(id<MTLDevice> device, id<MTLLibrary> library, NSMutableDictionary* row,
                   NSMutableDictionary* report, NSString* reportPath, bool explicitUint) {
    id<MTLFunction> function = [library newFunctionWithName:@"macro_integer_values"];
    require(function != nil, "authored kernel is absent");
    NSMutableDictionary* gpu = [NSMutableDictionary new];
    row[@"gpu"] = gpu;
    phase(row[@"case"], "pipeline_create");
    writeReport(report, reportPath);
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    gpu[@"pipeline_created"] = @(pipeline != nil);
    gpu[@"pipeline_error"] = errorInfo(error, row[@"case"], "pipeline");
    require(pipeline != nil && error == nil, "compute pipeline creation failed");
    id<MTLBuffer> output = [device newBufferWithLength:16 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    id<MTLBuffer> input = [device newBufferWithLength:2 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    require(output && input && output.contents && input.contents, "shared buffer creation failed");
    std::memset(output.contents, 0xcd, output.length);
    const uint32_t inputs[] = {0xffffffffu, 0x13579bdfu};
    std::memcpy(input.contents, inputs, sizeof(inputs));
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    require(queue && command && encoder, "compute command setup failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder setBuffer:input offset:0 atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    gpu[@"input_words"] = @[@(inputs[0]), @(inputs[1])];
    gpu[@"dispatch_threads"] = @1;
    gpu[@"initial_output_word"] = @(0xcdcdcdcdu);
    row[@"status"] = @"gpu_pending";
    writeReport(report, reportPath);
    phase(row[@"case"], "gpu_commit_wait");
    [command commit];
    [command waitUntilCompleted]; // The shared Python helper bounds the owned process, not Metal service work.
    gpu[@"command_status"] = command.status == MTLCommandBufferStatusCompleted ? @"completed" : @"failed";
    gpu[@"command_error"] = errorInfo(command.error, row[@"case"], "command");
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil, "GPU command failed");
    const uint32_t* words = static_cast<const uint32_t*>(output.contents);
    NSMutableArray* values = [NSMutableArray arrayWithCapacity:16];
    for (unsigned i = 0; i < 16; ++i) [values addObject:@(words[i])];
    gpu[@"words_u32"] = values;
    bool controls = words[3] == 0xffffffffu && words[4] == 4 && words[5] == 0
        && words[6] == inputs[0] && words[7] == 4 && words[8] == 0
        && words[9] == (inputs[1] ^ 0x2468ace0u) && words[10] == 0x4d434933u;
    for (unsigned i = 11; i < 16; ++i) controls = controls && words[i] == 0xcdcdcdcdu;
    gpu[@"authored_controls_match"] = @(controls);
    writeReport(report, reportPath);
    require(controls, "authored literal, input or guard readback control failed");
    if (explicitUint)
        require(words[0] == 0xffffffffu && words[1] == 4 && words[2] == 0, "explicit uint macro expectation failed");
    // The NSNumber and unsuffixed-string expression triples are observations.
    row[@"status"] = @"readback_observed";
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 5 || strcmp(argv[1], "--gpu-source") || strcmp(argv[3], "--report")) return 2;
        NSString* sourcePath = [NSString stringWithUTF8String:argv[2]];
        NSString* reportPath = [NSString stringWithUTF8String:argv[4]];
        if (!sourcePath || !reportPath || [NSFileManager.defaultManager fileExistsAtPath:reportPath]) return 2;
        NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
        NSMutableDictionary* report = [@{ @"schema": @1, @"scope": @"authored-u32-macro-gpu-expressions", @"status": @"running",
            @"sdk_max_allowed": @(__MAC_OS_X_VERSION_MAX_ALLOWED),
            @"os_version": @[@(os.majorVersion), @(os.minorVersion), @(os.patchVersion)],
            @"metal4_used": @NO, @"production_parity_verified": @NO, @"game_impact_verified": @NO,
            @"iphone_gpu_verified": @NO, @"all_macro_types_equivalent_verified": @NO,
            @"original_macro_type_recovery_verified": @NO, @"runtime_assignment_provenance_verified": @NO,
            @"runtime_macro_token_capture_verified": @NO, @"authored_legacy_gpu_execution_verified": @NO,
            @"cases": [NSMutableArray new],
            @"number_constructor": @{ @"factory": @"numberWithUnsignedInt", @"input_width_bits": @32, @"input_bits_hex": @"ffffffff" },
            @"requested_string_macros": @{ @"runtime_token_string": @"4294967295", @"runtime_uint_string": @"4294967295u" },
            @"token_scope": @"The unsuffixed string is a fixed previously observed token; this run does not capture NSNumber expansion." } mutableCopy];
        bool first = true;
        int result = 1;
        @try {
            try {
                writeReport(report, reportPath, first); first = false;
                if (os.majorVersion < 15) {
                    report[@"status"] = @"unsupported";
                    report[@"reason"] = @"macOS 15 or later is required for the authored options";
                    result = 78;
                } else {
                    NSString* source = readSource(sourcePath);
                    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                    report[@"device_present"] = @(device != nil);
                    report[@"device_name"] = device ? device.name : (id)NSNull.null;
                    if (!device) {
                        report[@"status"] = @"unsupported";
                        report[@"reason"] = @"No legacy MTLDevice is available; no compiler/GPU request was made";
                        result = 78;
                    } else {
                        NSArray* names = @[@"runtime_number", @"runtime_token_string", @"runtime_uint_string"];
                        NSArray* values = @[[NSNumber numberWithUnsignedInt:0xffffffffu], @"4294967295", @"4294967295u"];
                        NSArray* setters = @[@"languageVersion", @"mathMode", @"mathFloatingPointFunctions",
                                              @"preserveInvariance", @"optimizationLevel", @"preprocessorMacros"];
                        NSMutableArray* cases = report[@"cases"];
                        bool failed = false;
                        for (unsigned i = 0; i < 3; ++i) {
                            @autoreleasepool {
                                NSMutableDictionary* row = [@{ @"case": names[i], @"status": @"options_pending" } mutableCopy];
                                [cases addObject:row];
                                @try {
                                    try {
                                        MTLCompileOptions* options = authoredOptions(values[i]);
                                        require(options != nil, "authored compile options unavailable");
                                        row[@"effective_options"] = option_probe::snapshot(options, setters, false);
                                        row[@"integer_getter"] = i == 0
                                            ? @{ @"method": @"NSNumber.unsignedIntValue", @"bits_hex": option_probe::hexBits([values[i] unsignedIntValue], 32) }
                                            : @{ @"status": @"not_applicable_to_NSString" };
                                        row[@"status"] = @"library_pending";
                                        writeReport(report, reportPath);
                                        phase(names[i], "value_library_compile");
                                        NSError* error = nil;
                                        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
                                        row[@"value_library"] = @{ @"created": @(library != nil), @"error": errorInfo(error, names[i], "library") };
                                        if (!library) {
                                            row[@"status"] = @"compile_rejected";
                                            row[@"reason"] = error ? @"source library rejected with NSError" : @"source library returned nil without NSError";
                                            failed = true;
                                        } else {
                                            // Preserve warning NSError without rejecting a valid library.
                                            runGPU(device, library, row, report, reportPath, i == 2);
                                        }
                                    } catch (const std::exception& error) {
                                        row[@"status"] = @"failed";
                                        row[@"reason"] = [NSString stringWithUTF8String:error.what()] ?: @"native case failure";
                                        failed = true;
                                    }
                                } @catch (NSException* error) {
                                    row[@"status"] = @"failed";
                                    row[@"exception"] = @{ @"name": error.name ?: @"", @"reason": error.reason ?: @"" };
                                    failed = true;
                                }
                                writeReport(report, reportPath);
                            }
                        }
                        report[@"status"] = failed ? @"failed" : @"authored_expressions_observed";
                        report[@"authored_legacy_gpu_execution_verified"] = @(!failed);
                        if (failed) report[@"reason"] = @"At least one authored case failed; retained cases are not an equivalence success";
                        result = failed ? 1 : 0;
                    }
                }
            } catch (const std::exception& error) {
                report[@"status"] = @"failed";
                report[@"reason"] = [NSString stringWithUTF8String:error.what()] ?: @"native probe failure";
            }
        } @catch (NSException* error) {
            report[@"status"] = @"failed";
            report[@"exception"] = @{ @"name": error.name ?: @"", @"reason": error.reason ?: @"" };
        }
        try { writeReport(report, reportPath, first); }
        catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); result = 1; }
        return result;
    }
}
