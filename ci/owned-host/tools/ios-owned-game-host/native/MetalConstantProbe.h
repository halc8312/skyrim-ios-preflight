#ifndef OH_METAL_CONSTANT_PROBE_H
#define OH_METAL_CONSTANT_PROBE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Private-to-authored-probes API, unrelated to the guest ABI or the live CAMetalLayer.
// Same-thread context. Inputs are exclusively authored fixtures, never game bytes.
typedef struct OHConstantTimesV1 {
    uint64_t upload_ns, encode_ns, finish_ns, submit_ns, completion_ns, readback_ns, total_ns;
} OHConstantTimesV1;
void *OHMetalConstantCreateV1(void);
int32_t OHMetalConstantDestroyV1(void *context);
int32_t OHMetalConstantLimitsV1(uint64_t *out, size_t count);
int32_t OHMetalConstantPrepareV1(void *context, size_t output_bytes);
// Failure leaves caller output/times untouched. Caller owns a process deadline:
// blocking driver calls cannot be safely cancelled from inside this function.
int32_t OHMetalConstantRunV1(void *context, const uint32_t *records, size_t word_count,
    const uint8_t *values, size_t value_bytes, uint8_t *output, size_t output_bytes,
    OHConstantTimesV1 *times);
// Full shared fixture+WebGPU+native-Metal parity, selected only in public SDK tests.
typedef struct OHConstantComparisonV1 {
    uint32_t version, bytes, backend_a, backend_b;
    uint64_t cases, samples, compared_bytes;
    // Flags 1: upload strategies differ; 2: no GPU timing; 4: no game/display measurement.
    uint64_t flags, reserved;
} OHConstantComparisonV1;
int32_t OHRunPublicConstantComparisonV1(OHConstantComparisonV1 *out, size_t bytes);
#ifdef __cplusplus
}
static_assert(sizeof(OHConstantTimesV1) == 56);
static_assert(sizeof(OHConstantComparisonV1) == 56);
#endif
#endif
