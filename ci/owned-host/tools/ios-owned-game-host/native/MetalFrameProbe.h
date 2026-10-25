#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Authored offline shaders and immutable pass packets only. Same-thread contexts.
void *OHMetalFrameCreateV1(void);
int32_t OHMetalFrameDestroyV1(void *context);
// All pointer ranges are valid, disjoint and exclusively owned by the caller
// during this call. Failure leaves output untouched; an external process deadline
// is required because driver calls can block. No game or surface is attached.
int32_t OHMetalFrameRenderV1(void *context,const uint8_t *packet,size_t packet_bytes,uint8_t *output,size_t output_bytes);
typedef struct OHFrameComparisonV1 {
    uint32_t version,bytes,backend_a,backend_b;
    // flags: 1 immutable versions; 2 offline authored Xenos shaders;
    // 4 all pixels match the independent oracle and the other backend.
    uint64_t cases,renders,compared_bytes,flags;
} OHFrameComparisonV1;
int32_t OHRunPublicFrameComparisonV1(OHFrameComparisonV1 *out,size_t bytes);
#ifdef __cplusplus
}
static_assert(sizeof(OHFrameComparisonV1)==48);
#endif
