// Authored two-value legacy Metal experiment. No game or Metal 4 API is used.
// Inputs, option getters, diagnostic token spellings and GPU bits are separate.
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
    std::printf("macro_value case=%s phase=%s\n", name.UTF8String, value);
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

static NSDictionary* errorInfo(NSError* error) {
    if (!error) return @{ @"present": @NO };
    NSString* description = error.localizedDescription ?: @"";
    NSData* data = [description dataUsingEncoding:NSUTF8StringEncoding];
    NSMutableDictionary* result = [@{ @"present": @YES, @"domain": error.domain ?: @"", @"code": @(error.code),
        @"diagnostic_bytes": @(data.length), @"diagnostics_truncated": @(data.length > 65536) } mutableCopy];
    if (data.length > 65536)
        result[@"diagnostic_prefix_base64"] = [[data subdataWithRange:NSMakeRange(0, 65536)] base64EncodedStringWithOptions:0];
    else result[@"diagnostics"] = description;
    return result;
}

static float fromBits(uint32_t bits) {
    float value;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static NSDictionary* stringMacros() {
    return @{ @"F32_NEXT": @"1.00000011920928955078125f", @"F32_NEG_ZERO": @"-0.0f" };
}

static NSDictionary* numberMacros() {
    return @{ @"F32_NEXT": [NSNumber numberWithFloat:fromBits(0x3f800001u)],
              @"F32_NEG_ZERO": [NSNumber numberWithFloat:fromBits(0x80000000u)] };
}

static MTLCompileOptions* authoredOptions(NSDictionary* macros) {
    if (@available(macOS 15.0, *)) {
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        options.preserveInvariance = NO;
        options.optimizationLevel = MTLLibraryOptimizationLevelDefault;
        options.preprocessorMacros = macros;
        require(options.languageVersion == MTLLanguageVersion3_1 && options.mathMode == MTLMathModeSafe
            && options.mathFloatingPointFunctions == MTLMathFloatingPointFunctionsPrecise
            && !options.preserveInvariance && options.optimizationLevel == MTLLibraryOptimizationLevelDefault,
            "authored option getters differ from the requested profile");
        return options;
    }
    return nil;
}

static NSDictionary* floatGetters(NSDictionary* macros, bool numeric) {
    if (!numeric) return @{ @"status": @"not_applicable_to_NSString" };
    require(macros.count == 2, "numeric getter dictionary differs");
    NSMutableDictionary* bits = [NSMutableDictionary new];
    for (NSString* key in @[@"F32_NEXT", @"F32_NEG_ZERO"]) {
        id value = macros[key];
        require([value isKindOfClass:NSNumber.class], "NSNumber getter changed kind");
        bits[key] = option_probe::hexBits(option_probe::floatBits([(NSNumber*)value floatValue]), 32);
    }
    return @{ @"status": @"captured", @"method": @"NSNumber.floatValue", @"bits_hex": bits };
}

static void runGPU(id<MTLDevice> device, id<MTLLibrary> library, NSMutableDictionary* row,
                   NSMutableDictionary* report, NSString* reportPath) {
    id<MTLFunction> function = [library newFunctionWithName:@"macro_values"];
    require(function != nil, "authored kernel is absent");
    NSMutableDictionary* gpu = [NSMutableDictionary new];
    row[@"gpu"] = gpu;
    phase(row[@"case"], "pipeline_create");
    writeReport(report, reportPath);
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    gpu[@"pipeline_created"] = @(pipeline != nil);
    gpu[@"pipeline_error"] = errorInfo(error);
    require(pipeline != nil && error == nil, "compute pipeline creation failed");
    id<MTLBuffer> output = [device newBufferWithLength:8 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    id<MTLBuffer> input = [device newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
    require(output && input && output.contents && input.contents, "shared buffer creation failed");
    std::memset(output.contents, 0xcd, output.length);
    const uint32_t inputWord = 0x13579bdfu;
    std::memcpy(input.contents, &inputWord, sizeof(inputWord));
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    require(queue && command && encoder, "compute command setup failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder setBuffer:input offset:0 atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    gpu[@"input_word"] = @(inputWord);
    gpu[@"dispatch_threads"] = @1;
    gpu[@"initial_output_word"] = @(0xcdcdcdcdu);
    row[@"status"] = @"gpu_pending";
    writeReport(report, reportPath);
    phase(row[@"case"], "gpu_commit_wait");
    [command commit];
    [command waitUntilCompleted]; // The owning Python process supervisor bounds this wait.
    gpu[@"command_status"] = command.status == MTLCommandBufferStatusCompleted ? @"completed" : @"failed";
    gpu[@"command_error"] = errorInfo(command.error);
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil, "GPU command failed");
    const uint32_t* words = static_cast<const uint32_t*>(output.contents);
    NSMutableArray* values = [NSMutableArray arrayWithCapacity:8];
    for (unsigned i = 0; i < 8; ++i) [values addObject:@(words[i])];
    gpu[@"words_u32"] = values;
    gpu[@"macro_bits_hex"] = @{ @"F32_NEXT": option_probe::hexBits(words[0], 32),
                                 @"F32_NEG_ZERO": option_probe::hexBits(words[1], 32) };
    const bool controls = words[2] == 0x3f800001u && words[3] == 0x80000000u
        && words[4] == (inputWord ^ 0x2468ace0u) && words[5] == 0x4d435632u
        && words[6] == 0xcdcdcdcdu && words[7] == 0xcdcdcdcdu;
    gpu[@"authored_controls_match"] = @(controls);
    writeReport(report, reportPath);
    require(controls, "authored literal, input or guard readback control failed");
    // words[0:2] are observations, never an expected-number assertion.
    row[@"status"] = @"readback_observed";
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 7 || strcmp(argv[1], "--gpu-source") || strcmp(argv[3], "--diagnostic-source")
            || strcmp(argv[5], "--report")) return 2;
        NSString* gpuPath = [NSString stringWithUTF8String:argv[2]];
        NSString* diagnosticPath = [NSString stringWithUTF8String:argv[4]];
        NSString* reportPath = [NSString stringWithUTF8String:argv[6]];
        if (!gpuPath || !diagnosticPath || !reportPath || [NSFileManager.defaultManager fileExistsAtPath:reportPath]) return 2;
        NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
        NSMutableDictionary* report = [@{ @"schema": @1, @"scope": @"authored-two-float-macro-gpu-values", @"status": @"running",
            @"sdk_max_allowed": @(__MAC_OS_X_VERSION_MAX_ALLOWED),
            @"os_version": @[@(os.majorVersion), @(os.minorVersion), @(os.patchVersion)],
            @"metal4_used": @NO, @"production_parity_verified": @NO, @"game_impact_verified": @NO,
            @"iphone_gpu_verified": @NO, @"all_macro_types_equivalent_verified": @NO,
            @"original_macro_type_recovery_verified": @NO, @"runtime_assignment_provenance_verified": @NO,
            @"authored_legacy_gpu_execution_verified": @NO, @"cases": [NSMutableArray new],
            @"authored_float32_input_bits": @{ @"F32_NEXT": @"3f800001", @"F32_NEG_ZERO": @"80000000" },
            @"number_constructor": @"numberWithFloat from memcpy-reconstructed float32 bits",
            @"requested_string_macros": stringMacros() } mutableCopy];
        bool first = true;
        int result = 1;
        try {
            writeReport(report, reportPath, first); first = false;
            if (os.majorVersion < 15) {
                report[@"status"] = @"unsupported";
                report[@"reason"] = @"macOS 15 or later is required for the authored modern options";
                result = 78;
            } else {
                NSString* gpuSource = readSource(gpuPath);
                NSString* diagnosticSource = readSource(diagnosticPath);
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                report[@"device_present"] = @(device != nil);
                report[@"device_name"] = device ? device.name : (id)NSNull.null;
                if (!device) {
                    report[@"status"] = @"unsupported";
                    report[@"reason"] = @"No legacy MTLDevice is available; no compiler/GPU request was made";
                    result = 78;
                } else {
                    NSArray* names = @[@"runtime_number", @"runtime_string"];
                    NSArray* values = @[numberMacros(), stringMacros()];
                    NSArray* setters = @[@"languageVersion", @"mathMode", @"mathFloatingPointFunctions",
                                          @"preserveInvariance", @"optimizationLevel", @"preprocessorMacros"];
                    NSMutableArray* cases = report[@"cases"];
                    for (unsigned i = 0; i < 2; ++i) {
                        MTLCompileOptions* options = authoredOptions(values[i]);
                        require(options != nil, "authored compile options unavailable");
                        NSMutableDictionary* row = [@{ @"case": names[i], @"status": @"diagnostic_pending",
                            @"effective_options": option_probe::snapshot(options, setters, false),
                            @"float_getters": floatGetters(options.preprocessorMacros, i == 0),
                            @"requested_options_getters_match": @YES } mutableCopy];
                        [cases addObject:row];
                        writeReport(report, reportPath);
                        phase(names[i], "deliberate_token_rejection");
                        NSError* error = nil;
                        id<MTLLibrary> rejected = [device newLibraryWithSource:diagnosticSource options:options error:&error];
                        row[@"token_diagnostic"] = @{ @"library_created": @(rejected != nil), @"error": errorInfo(error) };
                        writeReport(report, reportPath);
                        require(!rejected && error, "expected diagnostic-only rejection did not occur");
                        row[@"status"] = @"library_pending";
                        writeReport(report, reportPath);
                        phase(names[i], "value_library_compile");
                        error = nil;
                        id<MTLLibrary> library = [device newLibraryWithSource:gpuSource options:options error:&error];
                        row[@"value_library"] = @{ @"created": @(library != nil), @"error": errorInfo(error) };
                        require(library != nil, "authored value library compilation failed");
                        runGPU(device, library, row, report, reportPath);
                        writeReport(report, reportPath);
                    }
                    report[@"status"] = @"authored_values_observed";
                    report[@"authored_legacy_gpu_execution_verified"] = @YES;
                    result = 0;
                }
            }
        } catch (const std::exception& error) {
            report[@"status"] = @"failed";
            report[@"reason"] = [NSString stringWithUTF8String:error.what()] ?: @"native probe failure";
            std::fprintf(stderr, "%s\n", error.what());
        }
        try { writeReport(report, reportPath, first); }
        catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); result = 1; }
        return result;
    }
}
