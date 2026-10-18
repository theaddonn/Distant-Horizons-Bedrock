// Spike B3: DH_Terrain fragment shader (see dh_terrain.vert.hlsl for the signature).
// FS uniform table (we define it): DhParams vec4 (fragment) @0, cbuf 16 B. No texture sampling.
// Output: vertex colour * DhParams.rgb; additionally lerped toward green by the smuggled
// frac(u_model[0][3]/64) (light.x): smuggled 0 -> unchanged, so chunk-origin-dependent green = smuggling works.
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
    float3 c = i.color.rgb * DhParams.rgb;
    float s = i.light.x;
    c = lerp(c, float3(0.0, 1.0, 0.0) * (c.r + c.g + c.b) * 0.3333, s);
    return float4(c, 1.0);
}
