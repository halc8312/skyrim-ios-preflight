/* Source-only strict LINK gate. main has NOT been executed. */
#include <stdint.h>
#include <stddef.h>
#include "webgpu.h"
#include "wgpu.h"
_Static_assert(sizeof(void *) == 8, "64-bit only");
_Static_assert(sizeof(WGPUChainedStruct) == 16, "Rust C ABI chain");
_Static_assert(sizeof(WGPUSurfaceSourceMetalLayer) == 24, "Rust C ABI Metal layer");
_Static_assert(offsetof(WGPUSurfaceSourceMetalLayer, layer) == 16, "Metal pointer offset");
_Static_assert(WGPUSType_SurfaceSourceMetalLayer == 4, "Metal chain stype");
extern uint64_t public_core_coexistence_probe(void);
int main(void) {
    return wgpuGetVersion() == UINT32_C(0x1d000101)
        && public_core_coexistence_probe() == 10 ? 0 : 1;
}
