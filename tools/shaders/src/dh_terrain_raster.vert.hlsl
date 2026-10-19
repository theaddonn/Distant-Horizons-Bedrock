// DH_Terrain vertex shader for the Rasterized* passes (RasterizedOpaque / RasterizedAlphaTest /
// RasterizedTransparent) of MeshFallbackPosUVNormalColor. These passes use a DIFFERENT vertex constant buffer than
// the Opaque/AlphaTest/Transparent passes (384 bytes, combined u_viewProj):
//   FogControl@0 u_viewProj@16 u_model[4]@80 FogColor@336 UVAnimation@352 ZShiftValue@368
// Reading them with the 464-byte "group A" layout (u_proj@0, u_view@64, u_model@144) gave garbage matrices and the
// "terrain bends into a dome" picture (2026-10-19). Everything else mirrors dh_terrain.vert.hlsl: same inputs,
// same varying signature, per-draw parameters smuggled in row 3 of u_model[0] (clip radius, fog start, fog end).
// Smuggled per-draw parameters (row 3 of u_model[0]): [3][0] = clipStart + fadeEnd * 4096 (clipStart = near clip
// radius in blocks, 0 = off; fadeEnd = vanilla render distance in blocks, the dither-fade band is [clipStart, fadeEnd]),
// [3][1] = fog start, [3][2] = fog end. Passed to the FS as light = (clipStart, fogStart, fogEnd, fadeEnd).
// uv0 = ((face + 1) / 8, packed light); see dh_terrain_light.hlsli.
#include "dh_terrain_light.hlsli"

cbuffer VertexUniforms : register(b0) {
    float4 FogControl;
    column_major float4x4 u_viewProj;
    column_major float4x4 u_model[4];
    float4 FogColor;
    float4 UVAnimation;
    float4 ZShiftValue;
};

struct VSIn {
    float4 color : COLOR0;
    float4 normal : NORMAL0;
    float3 position : POSITION0;
    float2 texCoords : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 clipPosition : COLOR3;
    float4 color : COLOR0;
    float4 fog : COLOR2;
    float4 light : COLOR1;
    nointerpolation float2 texCoords : TEXCOORD0; // see dh_terrain_light.hlsli (flat: per-quad)
    float3 worldPos : TEXCOORD3;
};

VSOut main(VSIn i) {
    VSOut o;
    float4x4 M = u_model[0];
    float4 p = float4(i.position, 1.0);
    // affine part only: row 3 carries the smuggled per-draw parameters and must not reach the transform
    float4 world = float4(dot(M[0], p), dot(M[1], p), dot(M[2], p), 1.0);
    float4 clip = mul(u_viewProj, world);
    o.pos = clip;
    o.clipPosition = clip;
    o.color = i.color;
    o.fog = FogColor;
    // [3][0] packs clipStart + fadeEnd * 4096 (integer blocks, each <= 4095, exact in a float up to 2^24).
    float fadeEnd = floor(M[3][0] / 4096.0 + 0.001);
    float clipStart = M[3][0] - fadeEnd * 4096.0;
    o.light = float4(clipStart, M[3][1], M[3][2], fadeEnd);
    // [3][3] selects the lighting path (see dh_terrain_light.hlsli and LodRenderer).
    o.texCoords = dhLightVarying(M[3][3], i.texCoords);
    o.worldPos = world.xyz;
    return o;
}
