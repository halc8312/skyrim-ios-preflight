// Fixed internal kernel, not a caller-supplied passthrough shader.
// Apple MSL specification (2026-06-04), section 5.2.3.5, table 5.7:
// [[stencil]] supplies the reference for the configured stencil operation.
// No early_fragment_tests; Replace/Always and full write mask are set in Rust.
#include <metal_stdlib>
using namespace metal;

struct StencilOut { uint reference [[stencil]]; };

vertex float4 stencil_vs(uint index [[vertex_id]]) {
    const float2 vertices[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(vertices[min(index, 2u)], 0, 1);
}

fragment StencilOut stencil_fs(float4 position [[position]],
                              texture2d<uint, access::read> scratch [[texture(0)]]) {
    uint2 p = uint2(position.xy);
    if (p.x >= scratch.get_width() || p.y >= scratch.get_height()) discard_fragment();
    uint value = scratch.read(p).r;
    if ((value & 0x100u) == 0u) discard_fragment();
    return StencilOut{value & 0xffu};
}

fragment StencilOut stencil_ms_fs(float4 position [[position]],
                                 uint sample [[sample_id]],
                                 texture2d_ms<uint, access::read> scratch [[texture(0)]]) {
    uint2 p = uint2(position.xy);
    if (p.x >= scratch.get_width() || p.y >= scratch.get_height() ||
        sample >= scratch.get_num_samples()) discard_fragment();
    uint value = scratch.read(p, sample).r;
    if ((value & 0x100u) == 0u) discard_fragment();
    return StencilOut{value & 0xffu};
}
