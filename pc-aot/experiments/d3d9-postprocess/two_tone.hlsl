// Original test effect for the Classic D3D9 pipeline (Shader Model 2.0).
sampler2D scene : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float4 sample = tex2D(scene, uv);
    float light = step(0.5, dot(sample.rgb, float3(0.25, 0.5, 0.25)));
    return float4(light, light, 1.0 - light, sample.a);
}
