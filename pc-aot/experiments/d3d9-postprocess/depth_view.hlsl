// SPDX-License-Identifier: GPL-3.0-only
sampler2D sceneDepth : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float z = tex2D(sceneDepth, uv).r;
    return float4(z, z, z, 1.0);
}
