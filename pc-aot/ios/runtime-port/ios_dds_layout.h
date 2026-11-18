#pragma once
#include "d3dx_image.h"
#ifdef __cplusplus
extern "C" {
#endif
int skyrim_ios_dds_cube_level(const NativeD3DXImage *, size_t, uint32_t face,
                            uint32_t level, size_t *offset, uint32_t *width,
                            uint32_t *height, uint32_t *depth, uint32_t *pitch,
                            uint32_t *rows);
#ifdef __cplusplus
}
#endif
