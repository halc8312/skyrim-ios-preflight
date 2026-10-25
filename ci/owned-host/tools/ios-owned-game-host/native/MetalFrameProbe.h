#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Authored offline shaders and immutable pass packets only. Same-thread contexts.
void *OHMetalFrameCreateV1(void);
void *OHMetalSampledFrameCreateV1(void);
void *OHMetalTargetFrameCreateV1(void);
int32_t OHMetalFrameDestroyV1(void *context);
// All pointer ranges are valid, disjoint and exclusively owned by the caller
// during this call. Failure leaves output untouched; an external process deadline
// is required because driver calls can block. No game or surface is attached.
int32_t OHMetalFrameRenderV1(void *context,const uint8_t *packet,size_t packet_bytes,uint8_t *output,size_t output_bytes);
int32_t OHMetalSampledFrameRenderV1(void *context,const uint8_t *packet,size_t packet_bytes,uint8_t *output,size_t output_bytes);
int32_t OHMetalTargetFrameRenderV1(void *context,const uint8_t *packet,size_t packet_bytes,uint8_t *output,size_t output_bytes);
// Opt-in frame-local allocation comparison. No cross-frame pooling or surface.
int32_t OHMetalTargetFrameRenderReusingV1(void *context,const uint8_t *packet,size_t packet_bytes,uint8_t *output,size_t output_bytes);
// Describes the exact allocation used by this completed render, not a separate
// prospective plan. Bytes are texel payload, not Metal driver's allocation size.
typedef struct OHFrameAllocationV1 {
    uint32_t version,bytes,policy,slots,targets,operations,reserved0,reserved1;
    uint64_t dedicated_bytes,allocated_bytes,color_load_elided,depth_load_elided,copy_elided;
    uint32_t mapping[32];
} OHFrameAllocationV1;
// policy 0=dedicated, 1=frame-local reuse. Three disjoint, valid ranges; failure
// leaves both outputs untouched. The audit output must be naturally aligned.
int32_t OHMetalTargetFrameRenderAuditedV1(void *context,const uint8_t *packet,size_t packet_bytes,
    uint32_t policy,uint8_t *output,size_t output_bytes,OHFrameAllocationV1 *audit,size_t audit_bytes);
typedef struct OHFrameComparisonV1 {
    uint32_t version,bytes,backend_a,backend_b;
    // flags: 1 immutable versions; 2 offline authored Xenos shaders;
    // 4 all pixels match the independent oracle and the other backend;
    // 8 target comparison additionally verifies dedicated/reused policies on both.
    uint64_t cases,renders,compared_bytes,flags;
} OHFrameComparisonV1;
int32_t OHRunPublicFrameComparisonV1(OHFrameComparisonV1 *out,size_t bytes);
int32_t OHRunPublicSampledFrameComparisonV1(OHFrameComparisonV1 *out,size_t bytes);
int32_t OHRunPublicTargetFrameComparisonV1(OHFrameComparisonV1 *out,size_t bytes);
#ifdef __cplusplus
}
static_assert(sizeof(OHFrameComparisonV1)==48);
static_assert(sizeof(OHFrameAllocationV1)==200);
#endif
