#include <metal_stdlib>
using namespace metal;
struct VertexOut { float4 position [[position]]; float3 color; };
vertex VertexOut probe_vertex(uint vertexID [[vertex_id]], constant float4 &state [[buffer(0)]]) {
    const float2 points[3] = { float2(0, 0.6), float2(-0.55, -0.4), float2(0.55, -0.4) };
    const float3 colors[3] = { float3(0.2, 0.9, 1), float3(1, 0.4, 0.3), float3(0.7, 1, 0.3) };
    float2 p = points[vertexID];
    float c = cos(state.z), s = sin(state.z);
    p = float2(c * p.x - s * p.y, s * p.x + c * p.y);
    p += state.xy;
    VertexOut out; out.position = float4(p.x / max(state.w, 0.1f), p.y, 0, 1); out.color = colors[vertexID]; return out;
}
fragment float4 probe_fragment(VertexOut in [[stage_in]]) { return float4(in.color, 1); }
