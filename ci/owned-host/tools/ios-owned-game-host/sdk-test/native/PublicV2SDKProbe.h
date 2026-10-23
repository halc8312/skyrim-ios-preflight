/* Fixture-tools only. Fixed self-authored fixture proof; never arbitrary memory. */
#ifndef PUBLIC_V2_SDK_PROBE_H
#define PUBLIC_V2_SDK_PROBE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OHPublicV2ProofV1 {
    uint32_t version, bytes, abi_version, layout_words;
    uint32_t host_bytes, requirements_bytes, provider_refusals, provider_preserved;
    uint32_t frequency, cleanup_count, jump_result, normal_result;
    uint32_t mmio_result, mmio_lock_word, imports_once, republish_ok;
} OHPublicV2ProofV1;
int32_t OHReadPublicV2ProofV1(uint64_t token,OHPublicV2ProofV1 *out,size_t bytes);
#ifdef __cplusplus
}
static_assert(sizeof(OHPublicV2ProofV1)==64);
#endif
#endif
