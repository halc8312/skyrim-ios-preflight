#ifndef OWNED_HOST_RENDERER_BRIDGE_H
#define OWNED_HOST_RENDERER_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OHRendererStatusV1 {
    uint32_t abi_version,struct_bytes,attachment,initialized,stopped,failed;
    uint64_t swap_requests,decoded_frames,gpu_errors,unrendered_swaps;
} OHRendererStatusV1;
/* Real CAMetalLayer created on the main thread. The app retains the layer for
 * process lifetime, including initialization failure and detached views. After
 * attachment the renderer owns configuration and applies layer setters on main.
 * Call from a worker while main stays available. Main-thread calls return -9
 * before changing attachment state. No retry/reset after an admitted attempt. */
int32_t OHAttachMetalLayerV1(uint64_t token,void *layer,uint32_t width,uint32_t height);
int32_t OHResizeMetalLayerV1(uint64_t token,uint32_t width,uint32_t height);
int32_t OHReadRendererStatusV1(uint64_t token,OHRendererStatusV1 *out,size_t bytes);
#ifdef __cplusplus
}
static_assert(sizeof(OHRendererStatusV1)==56&&alignof(OHRendererStatusV1)==8);
#endif
#endif
