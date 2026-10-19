// Spike B3: DH_Terrain fragment shader (see dh_terrain.vert.hlsl for the signature).
// FS uniform table (we define it): DhParams vec4 (fragment) @0, cbuf 16 B. No texture sampling.
// light.x = near clip radius (discard when the horizontal camera distance is below it; 0 = off), light.y/z = fog
// start/end in blocks, fog.rgb = vanilla's current fog colour. Output: vertex colour * DhParams.rgb, far fog.
cbuffer FragmentUniforms : register(b1) {
    float4 DhParams;
};

struct PSIn {
    float4 pos : SV_Position;
    float4 clipPosition : COLOR3;
    float4 color : COLOR0;
    float4 fog : COLOR2;
    float4 light : COLOR1;
    centroid float2 texCoords : TEXCOORD0;
    float3 worldPos : TEXCOORD3;
};

float4 main(PSIn i) : SV_Target0 {
    float d = length(i.worldPos.xz);
    if (i.light.x > 0.0 && d < i.light.x) discard;
    float3 c = i.color.rgb * DhParams.rgb;
    float fogStart = i.light.y, fogEnd = i.light.z;
    if (fogEnd > fogStart) c = lerp(c, i.fog.rgb, saturate((d - fogStart) / (fogEnd - fogStart)));
    return float4(c, 1.0);
}
