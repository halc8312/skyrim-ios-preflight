#ifndef PUBLIC_PROBE_WEBGPU_H
#define PUBLIC_PROBE_WEBGPU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PROBE_WEBGPU_EXPECTED_FLAGS UINT64_C(0x7ff)
typedef struct ProbeWebGPUReport {
    uint32_t abi_version, struct_size;
    uint64_t passed, failed;
    uint32_t native_version, backend_type, adapter_type, adapter_status;
    uint32_t device_status, surface_status, map_status, gpu_error_count;
    uint32_t device_lost_count, elapsed_ms, pixel_count, reserved;
    uint8_t rgba[64];
} ProbeWebGPUReport;

/* Public source-only WebGPU execution fixture, NOT Xenos/Skyrim execution.
 * Compile against pinned wgpu-native v29.0.1.1 headers, C++20, Apple SDK.
 * Run on the Apple host thread suitable for its layer (normally main thread).
 * A non-null layer must be a real, configured CAMetalLayer attached to a live
 * host view. Host retains it strongly until this call has returned, including
 * errors/cleanup; this fixture does not objc_retain/release or modify its owner.
 * Set valid drawable dimensions before calling; width/height are 1..4096.
 * Do not resize/detach/release the layer during the call. Output is aligned,
 * exclusively writable for sizeof(ProbeWebGPUReport), never retained.
 *
 * 5s is a TOTAL deadline for asynchronous polling. Driver-synchronous API calls
 * and upstream device destruction cannot be preempted here: XCTest/process
 * orchestration must impose an additional overall execution timeout. Timeout
 * callback state is heap-owned, avoiding a stack-userdata use-after-return.
 * A driver that never cancels a pending callback can retain that small state.
 *
 * Returns 0 only for every expected bit and all 16 red RGBA8 pixels.
 * -1 null output, -2 short byte capacity, -3 null layer/invalid dimensions,
 * -4 misaligned output, -5 concurrent fixture, -10..-18 failed GPU stage,
 * -19 async deadline, -20 validation/lost/deadline after cleanup, -21 C++ error.
 * Output is untouched for -1/-2/-4/-5; other results include ABI1/size136.
 * Unsupported Metal/surface/readback is FAILURE, never skipped as success.
 * Bits 0..10: version, instance, Metal surface, Metal non-CPU adapter, device,
 * surface capabilities, clear/present, offscreen clear/copy, pixel readback,
 * no validation/device loss, within 5s including observed cleanup time.
 * Offscreen format is RGBA8Unorm; 256B row pitch; every pixel=[255,0,0,255].
 * Surface is separately cleared green/presented, without claiming screenshot
 * correctness or surface readback when CopySrc is unsupported.
 */
int32_t probe_webgpu_metal(void *borrowed_layer, uint32_t width, uint32_t height,
                          ProbeWebGPUReport *out, size_t byte_capacity);
#ifdef __cplusplus
}
#endif
#endif
