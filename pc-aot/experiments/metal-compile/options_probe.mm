// Authored option objects only. No MTLDevice, library, source, compiler or GPU.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "options_snapshot.h"
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>

#if !defined(__MAC_OS_X_VERSION_MAX_ALLOWED) || __MAC_OS_X_VERSION_MAX_ALLOWED < 150000
#error This probe requires macOS SDK 15 or later
#endif

using namespace option_probe;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void appendCase(NSMutableArray* cases, NSString* name, MTLCompileOptions* options,
                       NSArray<NSString*>* setters, bool fresh = false, NSDictionary* declarations = nil) {
    NSMutableDictionary* row = [snapshot(options, setters, fresh) mutableCopy];
    row[@"case"] = name;
    row[@"authored_macro_declarations"] = declarations ?: @{};
    [cases addObject:row];
}

static NSDictionary* typedMacros(NSMutableDictionary* declared) {
    NSMutableDictionary* values = [NSMutableDictionary new];
    auto number = [&](NSString* name, NSNumber* value, NSString* kind, unsigned width, NSString* bits) {
        values[name] = value;
        declared[name] = @{ @"factory_kind": kind, @"declared_width_bits": @(width), @"declared_bits_hex": bits };
    };
    number(@"I8", [NSNumber numberWithChar:-7], @"signed_integer", 8, @"f9");
    number(@"I16", [NSNumber numberWithShort:-300], @"signed_integer", 16, @"fed4");
    number(@"I32", [NSNumber numberWithInt:-70000], @"signed_integer", 32, @"fffeee90");
    number(@"I64_MIN", [NSNumber numberWithLongLong:LLONG_MIN], @"signed_integer", 64, @"8000000000000000");
    number(@"U8", [NSNumber numberWithUnsignedChar:255], @"unsigned_integer", 8, @"ff");
    number(@"U16", [NSNumber numberWithUnsignedShort:65535], @"unsigned_integer", 16, @"ffff");
    number(@"U32", [NSNumber numberWithUnsignedInt:0xffffffffu], @"unsigned_integer", 32, @"ffffffff");
    number(@"U64_MAX", [NSNumber numberWithUnsignedLongLong:ULLONG_MAX], @"unsigned_integer", 64, @"ffffffffffffffff");
    number(@"F32", [NSNumber numberWithFloat:1.25f], @"binary_float", 32, hexBits(floatBits(1.25f), 32));
    number(@"F32_NEG_ZERO", [NSNumber numberWithFloat:-0.0f], @"binary_float", 32, @"80000000");
    number(@"F64", [NSNumber numberWithDouble:1.25], @"binary_float", 64, hexBits(doubleBits(1.25), 64));
    number(@"F64_NEG_ZERO", [NSNumber numberWithDouble:-0.0], @"binary_float", 64, @"8000000000000000");
    values[@"BOOL"] = @YES;
    declared[@"BOOL"] = @{ @"factory_kind": @"boolean", @"value": @YES };
    values[@"TOKEN_STRING"] = @"0x1234u";
    declared[@"TOKEN_STRING"] = @{ @"factory_kind": @"NSString", @"text": @"0x1234u" };
    return values;
}

static void writeReport(NSDictionary* report, NSString* path) {
    NSError* error = nil;
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
    require(bytes != nil, "option JSON serialization failed");
    require([bytes writeToFile:path options:NSDataWritingWithoutOverwriting error:&error], "report path exists or write failed");
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 3 || strcmp(argv[1], "--report") != 0) {
            fprintf(stderr, "usage: options_probe --report new-report.json\n");
            return 2;
        }
        NSString* path = [NSString stringWithUTF8String:argv[2]];
        if (!path || [NSFileManager.defaultManager fileExistsAtPath:path]) return 2;
        NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
        NSMutableDictionary* report = [@{
            @"schema": @1, @"status": @"failed", @"scope": @"authored-mtlcompileoptions-getters",
            @"sdk_max_allowed": @(__MAC_OS_X_VERSION_MAX_ALLOWED),
            @"os_version": @[@(os.majorVersion), @(os.minorVersion), @(os.patchVersion)],
            @"native_option_getters_verified": @NO, @"gpu_execution_verified": @NO,
            @"shader_compilation_verified": @NO, @"source_included": @NO,
            @"production_parity_verified": @NO, @"batch_profile_parity_verified": @NO,
            @"runtime_assignment_provenance_verified": @NO,
            @"original_macro_type_recovery_verified": @NO,
            @"macro_preprocessor_text_equivalence_verified": @NO,
            @"cases": [NSMutableArray new]
        } mutableCopy];
        int result = 1;
        try {
            require(os.majorVersion > 13 || (os.majorVersion == 13 && os.minorVersion >= 3), "macOS 13.3 or later is required");
            NSMutableArray* cases = report[@"cases"];
            appendCase(cases, @"fresh_defaults", [MTLCompileOptions new], @[], true);
            for (unsigned enabled = 0; enabled < 2; ++enabled) {
                MTLCompileOptions* options = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                options.fastMathEnabled = enabled != 0;
                require(options.fastMathEnabled == (enabled != 0), "legacy math getter differs from explicit setter");
#pragma clang diagnostic pop
                appendCase(cases, enabled ? @"legacy_fast" : @"legacy_safe", options, @[@"fastMathEnabled"]);
            }
            if (@available(macOS 15.0, *)) {
                const MTLMathMode modes[] = { MTLMathModeSafe, MTLMathModeRelaxed, MTLMathModeFast };
                NSArray* names = @[@"modern_safe", @"modern_relaxed", @"modern_fast"];
                for (unsigned i = 0; i < 3; ++i) {
                    MTLCompileOptions* options = [MTLCompileOptions new];
                    options.languageVersion = MTLLanguageVersion2_4;
                    options.mathMode = modes[i];
                    options.mathFloatingPointFunctions = i == 2 ? MTLMathFloatingPointFunctionsFast : MTLMathFloatingPointFunctionsPrecise;
                    options.preserveInvariance = YES;
                    options.optimizationLevel = MTLLibraryOptimizationLevelDefault;
                    require(options.languageVersion == MTLLanguageVersion2_4 && options.mathMode == modes[i]
                        && options.mathFloatingPointFunctions == (i == 2 ? MTLMathFloatingPointFunctionsFast : MTLMathFloatingPointFunctionsPrecise)
                        && options.preserveInvariance && options.optimizationLevel == MTLLibraryOptimizationLevelDefault,
                        "modern option getter differs from explicit setter");
                    appendCase(cases, names[i], options, @[@"languageVersion", @"mathMode", @"mathFloatingPointFunctions", @"preserveInvariance", @"optimizationLevel"]);
                }
            } else {
                report[@"modern_authored_cases"] = @"unsupported_os";
            }
            MTLCompileOptions* typed = [MTLCompileOptions new];
            NSMutableDictionary* declared = [NSMutableDictionary new];
            typed.preprocessorMacros = typedMacros(declared);
            appendCase(cases, @"typed_macros", typed, @[@"preprocessorMacros"], false, declared);
            MTLCompileOptions* empty = [MTLCompileOptions new];
            empty.preprocessorMacros = @{};
            empty.libraries = @[];
            empty.libraryType = MTLLibraryTypeExecutable;
            empty.installName = @"authored-option-snapshot";
            appendCase(cases, @"explicit_empty_and_output", empty, @[@"preprocessorMacros", @"libraries", @"libraryType", @"installName"]);
            // Unsupported value handling is exercised without passing it to Metal.
            require([macroValue([NSDate dateWithTimeIntervalSince1970:0])[@"status"] isEqual:@"unsupported_macro_value"],
                    "unsupported macro value was silently accepted");
            report[@"unsupported_macro_value_rejected"] = @YES;
            report[@"native_option_getters_verified"] = @YES;
            report[@"status"] = @"captured";
            result = 0;
        } catch (const std::exception& error) {
            report[@"failure"] = [NSString stringWithUTF8String:error.what()] ?: @"native failure";
            fprintf(stderr, "%s\n", error.what());
        }
        try { writeReport(report, path); }
        catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); result = 1; }
        return result;
    }
}
