// Isolated getter snapshot. This header is not connected to MoltenVK or the app.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreFoundation/CoreFoundation.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace option_probe {

inline NSString* hexBits(uint64_t bits, unsigned width) {
    return [NSString stringWithFormat:@"%0*llx", int(width / 4), (unsigned long long)bits];
}

inline uint32_t floatBits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline uint64_t doubleBits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// objCType describes NSNumber's returned representation. It does not recover
// the type/width of the value originally supplied to an NSNumber constructor.
inline NSDictionary* numberValue(NSNumber* number) {
    const char* type = number.objCType;
    NSString* encoding = type ? [NSString stringWithUTF8String:type] : nil;
    NSMutableDictionary* result = [@{ @"kind": @"NSNumber", @"objc_type": encoding ?: @"",
        @"original_declaration_recovered": @NO } mutableCopy];
    if (!type || strlen(type) != 1 || [number isKindOfClass:NSDecimalNumber.class]) {
        result[@"status"] = @"unsupported_number_representation";
        return result;
    }
    if (CFGetTypeID((__bridge CFTypeRef)number) == CFBooleanGetTypeID()) {
        result[@"status"] = @"captured";
        result[@"category"] = @"boolean";
        result[@"value"] = @(number.boolValue);
        return result;
    }
    bool signedInteger = false;
    unsigned width = 0;
    switch (type[0]) {
        case 'c': signedInteger = true; width = sizeof(char) * 8; break;
        case 's': signedInteger = true; width = sizeof(short) * 8; break;
        case 'i': signedInteger = true; width = sizeof(int) * 8; break;
        case 'l': signedInteger = true; width = sizeof(long) * 8; break;
        case 'q': signedInteger = true; width = sizeof(long long) * 8; break;
        case 'C': width = sizeof(unsigned char) * 8; break;
        case 'S': width = sizeof(unsigned short) * 8; break;
        case 'I': width = sizeof(unsigned int) * 8; break;
        case 'L': width = sizeof(unsigned long) * 8; break;
        case 'Q': width = sizeof(unsigned long long) * 8; break;
        case 'B': width = sizeof(bool) * 8; break;
        case 'f': case 'd': {
            bool narrow = type[0] == 'f';
            float f = number.floatValue;
            double d = number.doubleValue;
            result[@"status"] = @"captured";
            result[@"category"] = @"binary_float";
            result[@"observed_width_bits"] = @(narrow ? 32 : 64);
            result[@"bits_hex"] = narrow ? hexBits(floatBits(f), 32) : hexBits(doubleBits(d), 64);
            result[@"negative_zero"] = @(narrow ? f == 0 && std::signbit(f) : d == 0 && std::signbit(d));
            result[@"classification"] = (narrow ? std::isnan(f) : std::isnan(d)) ? @"nan"
                : (narrow ? std::isinf(f) : std::isinf(d)) ? @"infinity" : @"finite";
            return result; // No JSON floating-point value can erase -0 or precision.
        }
        default:
            result[@"status"] = @"unsupported_number_representation";
            return result;
    }
    uint64_t bits = signedInteger ? uint64_t(number.longLongValue) : uint64_t(number.unsignedLongLongValue);
    if (width < 64) bits &= (uint64_t(1) << width) - 1;
    result[@"status"] = @"captured";
    result[@"category"] = signedInteger ? @"signed_integer" : @"unsigned_integer";
    result[@"observed_width_bits"] = @(width);
    result[@"bits_hex"] = hexBits(bits, width);
    result[@"decimal"] = signedInteger ? [NSString stringWithFormat:@"%lld", number.longLongValue]
                                       : [NSString stringWithFormat:@"%llu", number.unsignedLongLongValue];
    return result;
}

inline NSDictionary* macroValue(id value) {
    if ([value isKindOfClass:NSNumber.class]) return numberValue(value);
    if ([value isKindOfClass:NSString.class] && [(NSString*)value length] <= 256)
        return @{ @"status": @"captured", @"kind": @"NSString", @"text": value };
    return @{ @"status": @"unsupported_macro_value" };
}

inline NSDictionary* macros(NSDictionary* values) {
    if (!values) return @{ @"is_nil": @YES, @"status": @"captured", @"entries": @[] };
    if (values.count > 64) return @{ @"is_nil": @NO, @"status": @"unsupported_macro_count" };
    for (id key in values) {
        if (![key isKindOfClass:NSString.class] || [(NSString*)key length] > 128)
            return @{ @"is_nil": @NO, @"status": @"unsupported_macro_key" };
    }
    NSMutableArray* entries = [NSMutableArray new];
    bool complete = true;
    for (NSString* key in [[values allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
        NSDictionary* value = macroValue(values[key]);
        complete &= [value[@"status"] isEqual:@"captured"];
        [entries addObject:@{ @"name": key, @"value": value }];
    }
    return @{ @"is_nil": @NO, @"status": complete ? @"captured" : @"contains_unsupported_value", @"entries": entries };
}

inline NSString* origin(NSString* name, NSSet<NSString*>* assigned, bool fresh) {
    if ([assigned containsObject:name]) return @"explicit_fixture_assignment";
    if (fresh) return @"default_observed";
    if (([name isEqual:@"mathMode"] || [name isEqual:@"mathFloatingPointFunctions"])
        && [assigned containsObject:@"fastMathEnabled"]) return @"affected_by_legacy_math_assignment";
    if ([name isEqual:@"fastMathEnabled"] && ([assigned containsObject:@"mathMode"]
        || [assigned containsObject:@"mathFloatingPointFunctions"])) return @"observed_after_modern_math_assignment";
    return @"not_explicitly_assigned_by_fixture";
}

inline NSDictionary* field(NSString* name, id value, NSSet<NSString*>* assigned, bool fresh) {
    return @{ @"availability": @"supported", @"origin": origin(name, assigned, fresh),
              @"value": value ?: NSNull.null };
}

inline NSDictionary* unavailable(NSString* reason) {
    return @{ @"availability": reason, @"origin": @"unobserved" };
}

inline NSDictionary* snapshot(MTLCompileOptions* options, NSArray<NSString*>* explicitSetters, bool fresh) {
    NSSet<NSString*>* assigned = [NSSet setWithArray:explicitSetters];
    NSMutableDictionary* fields = [NSMutableDictionary new];
    auto save = [&](NSString* name, id value) { fields[name] = field(name, value, assigned, fresh); };
    save(@"languageVersion", @{ @"raw": @((NSUInteger)options.languageVersion),
         @"known_name": options.languageVersion == MTLLanguageVersion2_4 ? @"2.4" : (id)NSNull.null });
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    save(@"fastMathEnabled", @(options.fastMathEnabled));
#pragma clang diagnostic pop
    save(@"preserveInvariance", @(options.preserveInvariance));
    save(@"optimizationLevel", @{ @"raw": @((NSInteger)options.optimizationLevel),
        @"is_default_enum": @(options.optimizationLevel == MTLLibraryOptimizationLevelDefault) });
    save(@"preprocessorMacros", macros(options.preprocessorMacros));
    save(@"libraryType", @((NSUInteger)options.libraryType));
    save(@"compileSymbolVisibility", @((NSUInteger)options.compileSymbolVisibility));
    save(@"allowReferencingUndefinedSymbols", @(options.allowReferencingUndefinedSymbols));
    save(@"maxTotalThreadsPerThreadgroup", @(options.maxTotalThreadsPerThreadgroup));
    save(@"installName", options.installName);
    NSArray* libraries = options.libraries;
    save(@"libraries", @{ @"is_nil": @(libraries == nil), @"count": @(libraries.count),
         @"status": libraries.count ? @"nonempty_library_identity_unverified" : @"empty_observed",
         @"nonempty_library_identity_verified": @NO });
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, *)) {
        NSString* mode = options.mathMode == MTLMathModeSafe ? @"safe" : options.mathMode == MTLMathModeRelaxed ? @"relaxed"
            : options.mathMode == MTLMathModeFast ? @"fast" : @"unknown";
        NSString* functions = options.mathFloatingPointFunctions == MTLMathFloatingPointFunctionsPrecise ? @"precise"
            : options.mathFloatingPointFunctions == MTLMathFloatingPointFunctionsFast ? @"fast" : @"unknown";
        save(@"mathMode", @{ @"raw": @((NSInteger)options.mathMode), @"known_name": mode });
        save(@"mathFloatingPointFunctions", @{ @"raw": @((NSInteger)options.mathFloatingPointFunctions), @"known_name": functions });
        save(@"enableLogging", @(options.enableLogging));
    } else {
        for (NSString* name in @[@"mathMode", @"mathFloatingPointFunctions", @"enableLogging"])
            fields[name] = unavailable(@"unsupported_os");
    }
#else
    for (NSString* name in @[@"mathMode", @"mathFloatingPointFunctions", @"enableLogging"])
        fields[name] = unavailable(@"unsupported_sdk");
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) {
        MTLSize size = options.requiredThreadsPerThreadgroup;
        save(@"requiredThreadsPerThreadgroup", @[@(size.width), @(size.height), @(size.depth)]);
    } else fields[@"requiredThreadsPerThreadgroup"] = unavailable(@"unsupported_os");
#else
    fields[@"requiredThreadsPerThreadgroup"] = unavailable(@"unsupported_sdk");
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if (@available(macOS 27.0, *))
        save(@"floatingPointConversionRoundingMode", @((NSInteger)options.floatingPointConversionRoundingMode));
    else fields[@"floatingPointConversionRoundingMode"] = unavailable(@"unsupported_os");
#else
    fields[@"floatingPointConversionRoundingMode"] = unavailable(@"unsupported_sdk");
#endif
    return @{ @"fields": fields, @"explicit_fixture_setters": explicitSetters,
              @"fresh_unmodified_object": @(fresh), @"runtime_assignment_provenance_verified": @NO };
}
} // namespace option_probe
