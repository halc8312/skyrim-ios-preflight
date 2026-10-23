/* Apple SDK-only host glue. This copy is not yet SDK-compiled or executed. */
#include "OwnedHostNative.h"
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <strings.h>
#include <stdlib.h>

namespace {
bool aligned(const void *p, size_t alignment) {
    return p && (reinterpret_cast<uintptr_t>(p) % alignment == 0);
}
bool copy_path(const char *source, char *out, size_t capacity, size_t *length) {
    if (!source || !out || !capacity) return false;
    const size_t count = strnlen(source, capacity);
    if (count == capacity || count > INT32_MAX) return false;
    memcpy(out, source, count + 1);
    if (length) *length = count;
    return true;
}
bool image_for(const void *symbol, const mach_header **header, intptr_t *slide,
               const char **path) {
    Dl_info info{};
    if (!symbol || !dladdr(symbol, &info) || !info.dli_fbase || !info.dli_fname)
        return false;
    const uint32_t count = _dyld_image_count();
    for (uint32_t index = 0; index < count; ++index) {
        const mach_header *current = _dyld_get_image_header(index);
        if (current != info.dli_fbase) continue;
        const char *name = _dyld_get_image_name(index);
        if (!name || !*name) return false;
        *header = current;
        *slide = _dyld_get_image_vmaddr_slide(index);
        *path = name;
        return true;
    }
    return false;
}
bool foreign_provider(const void *symbol, const mach_header *guest,
                      const mach_header *app, const mach_header **provider) {
    intptr_t slide{};
    const char *path{};
    return image_for(symbol, provider, &slide, &path) && *provider != guest &&
           *provider != app;
}
}

extern "C" uint32_t OHNativePlatformV1(void) {
#if defined(__aarch64__) && !defined(__arm64e__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST
#if TARGET_OS_SIMULATOR
    return 7;
#else
    return 2;
#endif
#else
    return 0;
#endif
}

extern "C" int32_t OHNativeHostLayoutV1(uint64_t *out, size_t capacity) {
    if (!aligned(out, alignof(uint64_t)) || capacity < 49) return -1;
#define FIELD(T, M) static_cast<uint64_t>(offsetof(T, M))
    const uint64_t values[] = {
        sizeof(PGRequirementsV1), alignof(PGRequirementsV1),
        FIELD(PGRequirementsV1, abi_version), FIELD(PGRequirementsV1, struct_size),
        FIELD(PGRequirementsV1, pointer_bits), FIELD(PGRequirementsV1, little_endian),
        FIELD(PGRequirementsV1, plain_arm64), FIELD(PGRequirementsV1, reserved),
        FIELD(PGRequirementsV1, context_layout), FIELD(PGRequirementsV1, jump_buffer_size),
        FIELD(PGRequirementsV1, jump_buffer_alignment), FIELD(PGRequirementsV1, ea_offset_count),
        FIELD(PGRequirementsV1, written_count),
        sizeof(PGMemoryV1), alignof(PGMemoryV1),
        FIELD(PGMemoryV1, ea_offsets), FIELD(PGMemoryV1, ea_offset_count),
        FIELD(PGMemoryV1, written), FIELD(PGMemoryV1, written_count),
        FIELD(PGMemoryV1, guest_base), FIELD(PGMemoryV1, guest_bytes),
        sizeof(PGHostCallbacksV1), alignof(PGHostCallbacksV1),
        FIELD(PGHostCallbacksV1, abi_version), FIELD(PGHostCallbacksV1, struct_size),
        FIELD(PGHostCallbacksV1, contracts), FIELD(PGHostCallbacksV1, jump_abi),
        FIELD(PGHostCallbacksV1, context_layout), FIELD(PGHostCallbacksV1, jump_buffer_size),
        FIELD(PGHostCallbacksV1, jump_buffer_alignment), FIELD(PGHostCallbacksV1, jump_pair_cookie),
        FIELD(PGHostCallbacksV1, unwind_abi), FIELD(PGHostCallbacksV1, unwind_reserved),
        FIELD(PGHostCallbacksV1, unwind_pair_cookie), FIELD(PGHostCallbacksV1, user),
        FIELD(PGHostCallbacksV1, memory), FIELD(PGHostCallbacksV1, timebase),
        FIELD(PGHostCallbacksV1, kernel_call), FIELD(PGHostCallbacksV1, fatal),
        FIELD(PGHostCallbacksV1, copy_memory), FIELD(PGHostCallbacksV1, zero_memory),
        FIELD(PGHostCallbacksV1, exp2_float), FIELD(PGHostCallbacksV1, log2_float),
        FIELD(PGHostCallbacksV1, native_setjmp), FIELD(PGHostCallbacksV1, native_longjmp),
        FIELD(PGHostCallbacksV1, native_personality), FIELD(PGHostCallbacksV1, native_unwind_resume),
        FIELD(PGHostCallbacksV1, stack_guard_seed), FIELD(PGHostCallbacksV1, reserved)
    };
#undef FIELD
    static_assert(sizeof(values) / sizeof(values[0]) == 49);
    memcpy(out, values, sizeof(values));
    return 49;
}

extern "C" int32_t OHNativeHostLayoutV2(uint64_t *out, size_t capacity) {
    if (!aligned(out, alignof(uint64_t)) || capacity < 51) return -1;
#define FIELD(T, M) static_cast<uint64_t>(offsetof(T, M))
    const uint64_t values[] = {
        sizeof(PGRequirementsV1), alignof(PGRequirementsV1),
        FIELD(PGRequirementsV1, abi_version), FIELD(PGRequirementsV1, struct_size),
        FIELD(PGRequirementsV1, pointer_bits), FIELD(PGRequirementsV1, little_endian),
        FIELD(PGRequirementsV1, plain_arm64), FIELD(PGRequirementsV1, reserved),
        FIELD(PGRequirementsV1, context_layout), FIELD(PGRequirementsV1, jump_buffer_size),
        FIELD(PGRequirementsV1, jump_buffer_alignment), FIELD(PGRequirementsV1, ea_offset_count),
        FIELD(PGRequirementsV1, written_count),
        sizeof(PGMemoryV1), alignof(PGMemoryV1),
        FIELD(PGMemoryV1, ea_offsets), FIELD(PGMemoryV1, ea_offset_count),
        FIELD(PGMemoryV1, written), FIELD(PGMemoryV1, written_count),
        FIELD(PGMemoryV1, guest_base), FIELD(PGMemoryV1, guest_bytes),
        sizeof(PGHostCallbacksV2), alignof(PGHostCallbacksV2),
        FIELD(PGHostCallbacksV2, abi_version), FIELD(PGHostCallbacksV2, struct_size),
        FIELD(PGHostCallbacksV2, contracts), FIELD(PGHostCallbacksV2, jump_abi),
        FIELD(PGHostCallbacksV2, context_layout), FIELD(PGHostCallbacksV2, jump_buffer_size),
        FIELD(PGHostCallbacksV2, jump_buffer_alignment), FIELD(PGHostCallbacksV2, jump_pair_cookie),
        FIELD(PGHostCallbacksV2, unwind_abi), FIELD(PGHostCallbacksV2, unwind_reserved),
        FIELD(PGHostCallbacksV2, unwind_pair_cookie), FIELD(PGHostCallbacksV2, user),
        FIELD(PGHostCallbacksV2, memory), FIELD(PGHostCallbacksV2, timebase),
        FIELD(PGHostCallbacksV2, kernel_call), FIELD(PGHostCallbacksV2, fatal),
        FIELD(PGHostCallbacksV2, copy_memory), FIELD(PGHostCallbacksV2, zero_memory),
        FIELD(PGHostCallbacksV2, exp2_float), FIELD(PGHostCallbacksV2, log2_float),
        FIELD(PGHostCallbacksV2, native_setjmp), FIELD(PGHostCallbacksV2, native_longjmp),
        FIELD(PGHostCallbacksV2, native_personality), FIELD(PGHostCallbacksV2, native_unwind_resume),
        FIELD(PGHostCallbacksV2, stack_guard_seed),
        FIELD(PGHostCallbacksV2, xma_read_u32), FIELD(PGHostCallbacksV2, xma_write_u32),
        FIELD(PGHostCallbacksV2, reserved)
    };
#undef FIELD
    static_assert(sizeof(values) / sizeof(values[0]) == 51);
    memcpy(out, values, sizeof(values));
    return 51;
}

extern "C" uint32_t OHNativeOSVersionV1(void) {
    @autoreleasepool {
        const NSOperatingSystemVersion version = NSProcessInfo.processInfo.operatingSystemVersion;
        if (version.majorVersion < 0 || version.majorVersion > 65535 ||
            version.minorVersion < 0 || version.minorVersion > 255 ||
            version.patchVersion < 0 || version.patchVersion > 255) return 0;
        return (static_cast<uint32_t>(version.majorVersion) << 16) |
               (static_cast<uint32_t>(version.minorVersion) << 8) |
               static_cast<uint32_t>(version.patchVersion);
    }
}

extern "C" int32_t OHNativeBundlePathV1(char *out, size_t capacity) {
    @autoreleasepool {
        size_t length{};
        const char *path = NSBundle.mainBundle.bundlePath.fileSystemRepresentation;
        return copy_path(path, out, capacity, &length) ? static_cast<int32_t>(length) : -1;
    }
}

extern "C" int32_t OHNativeSandboxPathV1(char *out, size_t capacity) {
    @autoreleasepool {
        size_t length{};
        const char *path = [NSHomeDirectory() fileSystemRepresentation];
        return copy_path(path, out, capacity, &length) ? static_cast<int32_t>(length) : -1;
    }
}

extern "C" void *OHNativeOpenGuestV1(const char *canonical_path) {
    if (!OHNativePlatformV1() || !canonical_path || !*canonical_path) return nullptr;
    return dlopen(canonical_path, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
}

extern "C" int32_t OHNativeLocateImageV1(const void *symbol, uintptr_t *header,
    int64_t *slide, char *path, size_t capacity) {
    if (!aligned(header, alignof(uintptr_t)) || !aligned(slide, alignof(int64_t))) return -1;
    const mach_header *image{};
    intptr_t image_slide{};
    const char *image_path{};
    if (!image_for(symbol, &image, &image_slide, &image_path) ||
        !copy_path(image_path, path, capacity, nullptr)) return -1;
    *header = reinterpret_cast<uintptr_t>(image);
    *slide = static_cast<int64_t>(image_slide);
    return 0;
}

extern "C" int32_t OHNativePrepareHostV1(PGHostCallbacksV1 *host, uint32_t bytes,
                                       const void *guest_header) {
    if (!OHNativePlatformV1() || !aligned(host, alignof(PGHostCallbacksV1)) ||
        bytes != sizeof(*host) || host->abi_version != PG_ABI_VERSION_1 ||
        host->struct_size != sizeof(*host) || host->contracts != PG_REQUIRED_CONTRACTS ||
        host->unwind_reserved) return -1;
    for (uint64_t value : host->reserved) if (value) return -1;
    const mach_header *guest{}, *app{};
    intptr_t slide{};
    const char *path{};
    if (!image_for(guest_header, &guest, &slide, &path) || guest != guest_header ||
        !image_for(reinterpret_cast<const void *>(&OHNativePrepareHostV1), &app, &slide, &path) ||
        guest == app) return -1;
    PGHostCallbacksV1 result = *host;
    result.copy_memory = &::memcpy;
    result.zero_memory = &::bzero;
    result.exp2_float = &::exp2f;
    result.log2_float = &::log2f;
    result.native_setjmp = reinterpret_cast<PGSetjmpV1>(&::setjmp);
    result.native_longjmp = reinterpret_cast<PGLongjmpV1>(&::longjmp);
    result.native_personality = reinterpret_cast<PGPersonalityV1>(dlsym(RTLD_DEFAULT, "__gxx_personality_v0"));
    result.native_unwind_resume = reinterpret_cast<PGUnwindResumeV1>(dlsym(RTLD_DEFAULT, "_Unwind_Resume"));
    const void *providers[] = {
        reinterpret_cast<const void *>(result.copy_memory), reinterpret_cast<const void *>(result.zero_memory),
        reinterpret_cast<const void *>(result.exp2_float), reinterpret_cast<const void *>(result.log2_float),
        reinterpret_cast<const void *>(result.native_setjmp), reinterpret_cast<const void *>(result.native_longjmp),
        reinterpret_cast<const void *>(result.native_personality), reinterpret_cast<const void *>(result.native_unwind_resume)
    };
    const mach_header *images[8]{};
    for (size_t i = 0; i < 8; ++i)
        if (!foreign_provider(providers[i], guest, app, &images[i])) return -1;
    if (images[4] != images[5]) return -1; // One native jump provider image.
    result.jump_abi = PG_JUMP_APPLE_ARM64_SETJMP_1;
    result.jump_buffer_size = sizeof(jmp_buf);
    result.jump_buffer_alignment = alignof(jmp_buf);
    result.jump_pair_cookie = reinterpret_cast<uintptr_t>(images[4]);
    result.unwind_abi = PG_UNWIND_APPLE_ARM64_DWARF_1;
    result.unwind_pair_cookie = reinterpret_cast<uintptr_t>(images[6]) ^
        (reinterpret_cast<uintptr_t>(images[7]) >> 1);
    if (!result.unwind_pair_cookie) return -1;
    arc4random_buf(&result.stack_guard_seed, sizeof(result.stack_guard_seed));
    if (!result.stack_guard_seed) return -1;
    *host = result;
    return 0;
}

/* Explicit V2 preparation; no V1/V2 pointer casts or ABI fallback. */
extern "C" int32_t OHNativePrepareHostV2(PGHostCallbacksV2 *host, uint32_t bytes,
                                       const void *guest_header) {
    if (!OHNativePlatformV1() || !aligned(host, alignof(PGHostCallbacksV2)) ||
        bytes != sizeof(*host) || host->abi_version != PG_ABI_VERSION_2 ||
        host->struct_size != sizeof(*host) || host->contracts != PG_V2_REQUIRED_CONTRACTS ||
        host->unwind_reserved || !host->xma_read_u32 || !host->xma_write_u32) return -1;
    for (uint64_t value : host->reserved) if (value) return -1;
    const mach_header *guest{}, *app{};
    intptr_t slide{};
    const char *path{};
    if (!image_for(guest_header, &guest, &slide, &path) || guest != guest_header ||
        !image_for(reinterpret_cast<const void *>(&OHNativePrepareHostV2), &app, &slide, &path) ||
        guest == app) return -1;
    PGHostCallbacksV2 result = *host;
    result.copy_memory = &::memcpy;
    result.zero_memory = &::bzero;
    result.exp2_float = &::exp2f;
    result.log2_float = &::log2f;
    result.native_setjmp = reinterpret_cast<PGSetjmpV1>(&::setjmp);
    result.native_longjmp = reinterpret_cast<PGLongjmpV1>(&::longjmp);
    result.native_personality = reinterpret_cast<PGPersonalityV1>(dlsym(RTLD_DEFAULT, "__gxx_personality_v0"));
    result.native_unwind_resume = reinterpret_cast<PGUnwindResumeV1>(dlsym(RTLD_DEFAULT, "_Unwind_Resume"));
    const void *providers[] = {
        reinterpret_cast<const void *>(result.copy_memory), reinterpret_cast<const void *>(result.zero_memory),
        reinterpret_cast<const void *>(result.exp2_float), reinterpret_cast<const void *>(result.log2_float),
        reinterpret_cast<const void *>(result.native_setjmp), reinterpret_cast<const void *>(result.native_longjmp),
        reinterpret_cast<const void *>(result.native_personality), reinterpret_cast<const void *>(result.native_unwind_resume)
    };
    const mach_header *images[8]{};
    for (size_t i = 0; i < 8; ++i)
        if (!foreign_provider(providers[i], guest, app, &images[i])) return -1;
    if (images[4] != images[5]) return -1; // One native jump provider image.
    result.jump_abi = PG_JUMP_APPLE_ARM64_SETJMP_1;
    result.jump_buffer_size = sizeof(jmp_buf);
    result.jump_buffer_alignment = alignof(jmp_buf);
    result.jump_pair_cookie = reinterpret_cast<uintptr_t>(images[4]);
    result.unwind_abi = PG_UNWIND_APPLE_ARM64_DWARF_1;
    result.unwind_pair_cookie = reinterpret_cast<uintptr_t>(images[6]) ^
        (reinterpret_cast<uintptr_t>(images[7]) >> 1);
    if (!result.unwind_pair_cookie) return -1;
    arc4random_buf(&result.stack_guard_seed, sizeof(result.stack_guard_seed));
    if (!result.stack_guard_seed) return -1;
    *host = result;
    return 0;
}
