// Isolated authored diagnostics. Both library compilations MUST be rejected.
// No pipeline, command queue, dispatch, Metal 4 or game data is used.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "options_snapshot.h"
#include <climits>
#include <cstdio>
#include <stdexcept>

#if !defined(__MAC_OS_X_VERSION_MAX_ALLOWED) || __MAC_OS_X_VERSION_MAX_ALLOWED < 150000
#error This diagnostic probe requires macOS SDK 15 or later
#endif

static void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

static NSDictionary* stringMacros() {
    return @{ @"U64_MAX": @"18446744073709551615ull", @"U64_HIGH": @"9223372036854775808ull",
        @"I64_MIN": @"-9223372036854775808ll", @"U32_MAX": @"4294967295u",
        @"F32_NEG_ZERO": @"-0.0f", @"F32_NEXT": @"1.00000011920928955078125f",
        @"BOOL_VALUE": @"true", @"TOKEN_STRING": @"0x1234u" };
}

static NSDictionary* numberMacros() {
    return @{ @"U64_MAX": [NSNumber numberWithUnsignedLongLong:ULLONG_MAX],
        @"U64_HIGH": [NSNumber numberWithUnsignedLongLong:(1ULL << 63)],
        @"I64_MIN": [NSNumber numberWithLongLong:LLONG_MIN],
        @"U32_MAX": [NSNumber numberWithUnsignedInt:0xffffffffu],
        @"F32_NEG_ZERO": [NSNumber numberWithFloat:-0.0f],
        @"F32_NEXT": [NSNumber numberWithFloat:1.0f + 0x1p-23f],
        @"BOOL_VALUE": @YES, @"TOKEN_STRING": @"0x1234u" };
}

static NSDictionary* declarations() {
    return @{ @"U64_MAX": @{ @"factory": @"numberWithUnsignedLongLong", @"input_width_bits": @64, @"input_bits_hex": @"ffffffffffffffff" },
        @"U64_HIGH": @{ @"factory": @"numberWithUnsignedLongLong", @"input_width_bits": @64, @"input_bits_hex": @"8000000000000000" },
        @"I64_MIN": @{ @"factory": @"numberWithLongLong", @"input_width_bits": @64, @"input_bits_hex": @"8000000000000000" },
        @"U32_MAX": @{ @"factory": @"numberWithUnsignedInt", @"input_width_bits": @32, @"input_bits_hex": @"ffffffff" },
        @"F32_NEG_ZERO": @{ @"factory": @"numberWithFloat", @"input_width_bits": @32, @"input_bits_hex": @"80000000" },
        @"F32_NEXT": @{ @"factory": @"numberWithFloat", @"input_width_bits": @32, @"input_bits_hex": @"3f800001" },
        @"BOOL_VALUE": @{ @"factory": @"boolean", @"input_value": @YES },
        @"TOKEN_STRING": @{ @"factory": @"NSString", @"input_text": @"0x1234u" } };
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
            "authored option getters differ from requested profile");
        return options;
    }
    return nil;
}

static void writeReport(NSDictionary* report, NSString* path, bool first) {
    NSError* error = nil;
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
    require(bytes != nil, "report serialization failed");
    require([bytes writeToFile:path options:first ? NSDataWritingWithoutOverwriting : NSDataWritingAtomic error:&error],
            "report write failed");
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 5 || strcmp(argv[1], "--source") || strcmp(argv[3], "--report")) return 2;
        NSString* sourcePath = [NSString stringWithUTF8String:argv[2]];
        NSString* reportPath = [NSString stringWithUTF8String:argv[4]];
        if (!sourcePath || !reportPath || [NSFileManager.defaultManager fileExistsAtPath:reportPath]) return 2;
        NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
        NSMutableDictionary* report = [@{ @"schema": @1, @"scope": @"authored-static-assert-token-diagnostics",
            @"status": @"running", @"sdk_max_allowed": @(__MAC_OS_X_VERSION_MAX_ALLOWED),
            @"os_version": @[@(os.majorVersion), @(os.minorVersion), @(os.patchVersion)],
            @"expected_compilation_failure": @YES, @"shader_compilation_success_verified": @NO,
            @"gpu_execution_verified": @NO, @"metal4_used": @NO, @"production_parity_verified": @NO,
            @"typed_value_equivalence_verified": @NO, @"original_macro_type_recovery_verified": @NO,
            @"runtime_assignment_provenance_verified": @NO, @"cases": [NSMutableArray new],
            @"authored_constructor_arguments": declarations(), @"requested_string_macros": stringMacros() } mutableCopy];
        int result = 1;
        bool first = true;
        try {
            writeReport(report, reportPath, first); first = false;
            if (os.majorVersion < 15) {
                report[@"status"] = @"unsupported";
                report[@"reason"] = @"macOS 15 or later is required for the authored modern profile";
                result = 78;
            } else {
                NSData* bytes = [NSData dataWithContentsOfFile:sourcePath];
                require(bytes && bytes.length > 0 && bytes.length <= 16384, "authored source size invalid");
                NSString* source = [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
                require(source != nil, "authored source is not UTF-8");
                report[@"source_bytes"] = @(bytes.length);
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                report[@"device_present"] = @(device != nil);
                report[@"device_name"] = device ? device.name : (id)NSNull.null;
                if (!device) {
                    report[@"status"] = @"unsupported";
                    report[@"reason"] = @"No legacy MTLDevice is available; no compiler request was made";
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
                        NSMutableDictionary* row = [@{ @"case": names[i], @"status": @"compiling",
                            @"effective_options": option_probe::snapshot(options, setters, false),
                            @"requested_options_getters_match": @YES } mutableCopy];
                        [cases addObject:row];
                        writeReport(report, reportPath, false);
                        printf("macro_diagnostic case=%s phase=begin\n", [names[i] UTF8String]); fflush(stdout);
                        NSError* error = nil;
                        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
                        row[@"library_created"] = @(library != nil);
                        row[@"error_present"] = @(error != nil);
                        row[@"error_domain"] = error ? error.domain : (id)NSNull.null;
                        row[@"error_code"] = error ? @(error.code) : (id)NSNull.null;
                        NSString* description = error ? error.localizedDescription : @"";
                        NSData* diagnosticBytes = [description dataUsingEncoding:NSUTF8StringEncoding];
                        row[@"diagnostic_bytes"] = @(diagnosticBytes.length);
                        if (diagnosticBytes.length > 65536) {
                            row[@"diagnostics_truncated"] = @YES;
                            row[@"diagnostic_prefix_base64"] = [[diagnosticBytes subdataWithRange:NSMakeRange(0, 65536)] base64EncodedStringWithOptions:0];
                            row[@"status"] = @"unsupported_diagnostic_size";
                            writeReport(report, reportPath, false);
                            require(false, "diagnostics exceed the capture limit; bounded prefix retained");
                        }
                        row[@"diagnostics_truncated"] = @NO;
                        row[@"diagnostics"] = description;
                        row[@"status"] = !library && error ? @"deliberately_rejected" : @"unexpected_result";
                        writeReport(report, reportPath, false);
                        require(!library && error, "expected deliberate compiler rejection did not occur");
                    }
                    report[@"status"] = @"deliberate_rejections_observed";
                    result = 0; // The observer finished; shader compilation did not succeed.
                }
            }
        } catch (const std::exception& error) {
            report[@"status"] = @"failed";
            report[@"reason"] = [NSString stringWithUTF8String:error.what()] ?: @"native probe failure";
            fprintf(stderr, "%s\n", error.what());
        }
        try { writeReport(report, reportPath, first); }
        catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); result = 1; }
        return result;
    }
}
