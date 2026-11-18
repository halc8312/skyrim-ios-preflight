// Authored fixture. No game shaders or data are included.
#include <metal_stdlib>
using namespace metal;

#ifndef PROBE_BIAS
#error PROBE_BIAS must be supplied by the recorded compile options
#endif

constant uint probe_scale [[function_constant(0)]];

struct ProbeVertex {
    float4 position [[position]];
};

vertex ProbeVertex probe_vertex(uint id [[vertex_id]]) {
    const float2 positions[3] = {
        float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)
    };
    ProbeVertex result;
    result.position = float4(positions[id], 0.0f, 1.0f);
    return result;
}

fragment float4 probe_fragment() {
    return float4(float(PROBE_BIAS) / 16.0f,
                  float(probe_scale) / 16.0f, 0.5f, 1.0f);
}

kernel void probe_kernel(device uint4* output [[buffer(0)]],
                         uint id [[thread_position_in_grid]]) {
    if (id < 4) {
        output[id] = uint4(id + uint(PROBE_BIAS), (id + 1) * probe_scale,
                          uint(PROBE_BIAS) + probe_scale, 0x12345678u);
    }
}
