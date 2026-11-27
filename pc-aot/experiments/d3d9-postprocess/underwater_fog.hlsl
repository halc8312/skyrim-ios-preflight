// SPDX-License-Identifier: GPL-3.0-only
// Controlled water pass. Scene color may be refracted while depth stays at the original pixel.
sampler2D sceneColor : register(s0);
sampler2D sceneDepth : register(s1);
float4 fogColor : register(c0);
// c1 contains 1/near, 1/far - 1/near, fog start and 1/(fog end - fog start).
float4 projectionAndFog : register(c1);
float4 refractionOffset : register(c2);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float4 scene = tex2D(sceneColor, uv + refractionOffset.xy);
    float z = tex2D(sceneDepth, uv).r;
    float distance = rcp(projectionAndFog.x + z * projectionAndFog.y);
    float amount = saturate((distance - projectionAndFog.z) * projectionAndFog.w);
    return float4(lerp(scene.rgb, fogColor.rgb, amount), scene.a);
}
