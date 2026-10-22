#ifndef OWNED_HOST_NATIVE_TRACE_SDK_PROBE_H
#define OWNED_HOST_NATIVE_TRACE_SDK_PROBE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Test-only API. It exposes counts only, never raw PCs, image paths or symbols.
typedef struct OHNativeTraceSDKResult {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t captured_count;
    uint32_t expected_parent_count;
    uint32_t matched_parent_count;
    uint32_t rx_parent_count;
    uint32_t flags;
    uint32_t reserved;
} OHNativeTraceSDKResult;
int32_t OHTestNativeTraceSDKV1(OHNativeTraceSDKResult *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
