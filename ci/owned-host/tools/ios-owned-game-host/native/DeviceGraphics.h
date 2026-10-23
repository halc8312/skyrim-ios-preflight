#ifndef OWNED_HOST_DEVICE_GRAPHICS_H
#define OWNED_HOST_DEVICE_GRAPHICS_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OHBootGraphicsV1 {
    uint32_t version,struct_bytes,video_width,video_height,internal_scale,mirror_mib,mobile_texture_budget,reserved;
} OHBootGraphicsV1;
typedef struct OHPresentationSettingsV1 {
    uint32_t version,struct_bytes,frame_limit,output_scale_percent,filter,sharpness,reserved0,reserved1;
} OHPresentationSettingsV1;
typedef struct OHGraphicsStatusV1 {
    uint32_t version,struct_bytes,video_width,video_height,internal_scale,mirror_mib,mobile_texture_budget,boot_frozen;
    uint32_t requested_frame_limit,requested_output_scale_percent,requested_filter,requested_sharpness;
    uint32_t applied_frame_limit,applied_output_scale_percent,applied_filter,applied_sharpness;
    uint32_t requested_generation,applied_generation,output_width,output_height,source_width,source_height;
    uint64_t presented_frames,skipped_frames,decoded_frames,reused_frames;
} OHGraphicsStatusV1;
/* Full validated tuples; no guest pointers. Startup settings freeze at renderer
 * creation. Scale1..3 is real integer EDRAM supersampling; output scale affects
 * the final display pass only. FPS limits never skip guest draws or resolves. */
int32_t OHConfigureBootGraphicsV1(uint64_t token,const OHBootGraphicsV1 *settings,size_t bytes);
int32_t OHSetPresentationSettingsV1(uint64_t token,const OHPresentationSettingsV1 *settings,size_t bytes);
int32_t OHReadGraphicsStatusV1(uint64_t token,OHGraphicsStatusV1 *out,size_t bytes);
#ifdef __cplusplus
}
static_assert(sizeof(OHBootGraphicsV1)==32&&sizeof(OHPresentationSettingsV1)==32);
static_assert(sizeof(OHGraphicsStatusV1)==120&&alignof(OHGraphicsStatusV1)==8);
#endif
#endif
