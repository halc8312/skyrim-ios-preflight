enum ProbeShaderSource {
    // Hand-written test shader. It is compiled through the standard Metal API,
    // which is independent of executable CPU-memory/JIT entitlements.
    static let msl = """
    #include <metal_stdlib>
    using namespace metal;

    struct ProbeVertex {
        float4 position [[position]];
        float2 uv;
    };

    vertex ProbeVertex probe_vertex(uint index [[vertex_id]]) {
        const float2 position[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
        const float2 uv[] = {float2(0, 1), float2(2, 1), float2(0, -1)};
        return {float4(position[index], 0, 1), uv[index]};
    }

    fragment float4 probe_fragment(ProbeVertex in [[stage_in]], texture2d<float> image [[texture(0)]]) {
        constexpr sampler nearest(coord::normalized, address::clamp_to_edge, filter::nearest);
        return image.sample(nearest, in.uv);
    }
    """
}
