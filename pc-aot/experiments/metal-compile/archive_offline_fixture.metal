// Authored constant-free compute/render archive fixture. No game content.
#include <metal_stdlib>
using namespace metal;

kernel void archive_recorded(device uint4* output [[buffer(0)]],
                            uint id [[thread_position_in_grid]]) {
    if (id < 4)
        output[id] = uint4(id + 3u, (id + 1u) * 7u, 10u, 0x12345678u);
}

struct AuthoredVertex { float4 position [[position]]; };
vertex AuthoredVertex archive_vertex(uint vertexID [[vertex_id]]) {
    float2 point = vertexID == 0u ? float2(-1.0f, -1.0f)
        : vertexID == 1u ? float2(3.0f, -1.0f) : float2(-1.0f, 3.0f);
    AuthoredVertex value;
    value.position = float4(point, 0.0f, 1.0f);
    return value;
}
fragment float4 archive_fragment() { return float4(0.25f, 0.5f, 0.75f, 1.0f); }
