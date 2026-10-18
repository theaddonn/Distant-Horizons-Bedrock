// Spike B2: replacement fragment shader for RenderChunk / Opaque. Keeps the VANILLA vertex blob untouched, so the
// input struct below mirrors the vanilla vertex OUTPUT signature (from `dxc -dumpbin` of Opaque v0 SM65 VS):
//   SV_Position, COLOR0, TEXCOORD2.xy + TEXCOORD1.zw (packed in one register), COLOR2, TEXCOORD0 (centroid), TEXCOORD3.xyz
// Names (bgfx/matbin): color0, ditheringAndMaskTinting(TEXCOORD2), lightmapUV(TEXCOORD1), fog(COLOR2), texcoord0, worldPos.
// Resources (vanilla SM65 FS reflection): t0/s0 = LightMapTexture, t1/s1 = MatTexture.
// Build: dxc -T ps_6_5 -E main | fxc_lite ps_5_0 / ps_4_0. Define FLAT for a texture-free fallback.
Texture2D MatTex : register(t1);
SamplerState MatSamp : register(s1);

struct PSIn {
    float4 pos : SV_Position;
    float4 color0 : COLOR0;
    float2 ditheringAndMaskTinting : TEXCOORD2;
    float2 lightmapUV : TEXCOORD1;
    float4 fog : COLOR2;
    centroid float2 texcoord0 : TEXCOORD0;
    float3 worldPos : TEXCOORD3;
};

float4 main(PSIn i) : SV_Target0 {
#ifdef FLAT
    return float4(1.0, 0.5, 0.5, 1.0);
#else
    float4 t = MatTex.Sample(MatSamp, i.texcoord0);
    // terrain atlas * vertex colour (AO / biome tint) * pink; alpha forced opaque
    return float4(float3(1.0, 0.5, 0.5) * t.rgb * i.color0.rgb, 1.0);
#endif
}
