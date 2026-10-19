// Spike B3: DH_Terrain vertex shader. Material cloned from MeshFallbackPosUVNormalColor / Opaque; keeps that pass's
// VS inputs (position, normal, color, texCoords) and its VS->FS varying signature:
//   SV_Position, COLOR3 clipPosition, COLOR0 color, COLOR2 fog, COLOR1 light, TEXCOORD0 texCoords (centroid), TEXCOORD3 worldPos
// Constant buffer layout == vanilla MeshFallback group A (cbuf 464 B): u_proj@0 u_view@64 FogControl@128 u_model[4]@144
// FogColor@400 SubPixelOffset@416 UVAnimation@432 ZShiftValue@448 (only u_proj/u_view/u_model are read).
// Per-draw parameters are smuggled through the projective row of u_model[0] (row 3, entries 0..2, zero in an affine
// matrix): [3][0] = near clip radius (blocks, 0 = off), [3][1] = fog start, [3][2] = fog end. They are passed to the FS
// in `light.xyz`; the geometry is transformed with row 3 reset to (0,0,0,1) so they never affect positions.
// Smuggled per-draw parameters (row 3 of u_model[0]): [3][0] = clipStart + fadeEnd * 4096 (clipStart = near clip
// radius in blocks, 0 = off; fadeEnd = vanilla render distance in blocks, the dither-fade band is [clipStart, fadeEnd]),
// [3][1] = fog start, [3][2] = fog end. Passed to the FS as light = (clipStart, fogStart, fogEnd, fadeEnd).
// texCoords (uv0) = ((face + 1) / 8 with face 0 down,1 up,2 north,3 south,4 west,5 east ; packed light (sky*16+block)/255).
// The mesh MUST enable the uv0 vertex field (MeshUploader: mFieldEnabled[4]); without it TEXCOORD0 reads 0.
#include "dh_terrain_light.hlsli"

cbuffer VertexUniforms : register(b0) {
    column_major float4x4 u_proj;
    column_major float4x4 u_view;
    float4 FogControl;
    column_major float4x4 u_model[4];
    float4 FogColor;
    float4 SubPixelOffset;
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
    float4 world = float4(dot(M[0], p), dot(M[1], p), dot(M[2], p), 1.0);
    float4 clip = mul(u_proj, mul(u_view, world));
    o.pos = clip;
    o.clipPosition = clip;
    o.color = i.color;
    o.fog = FogColor;
    // [3][0] packs clipStart + fadeEnd * 4096 (integer blocks, each <= 4095, exact in a float up to 2^24), except for
    // the cloud draw (marker [3][3] = 3), where it carries the packed tint.
    float fadeEnd = floor(M[3][0] / 4096.0 + 0.001);
    float clipStart = M[3][0] - fadeEnd * 4096.0;
    if (M[3][3] > 2.5) {
        // DH cloud layer (marker 3, > 2.5; still unlit for the light helper): no near clip / fade;
        // [3][0] carries the per-draw tint instead, packed as
        // r + g * 256 + b * 65536 (8 bits each, exact in a float), so the mesh stays white and the tint follows the
        // sky every frame without a rebuild (Run 13b).
        float pk = M[3][0] + 0.5;
        float tb = floor(pk / 65536.0);
        float tg = floor((pk - tb * 65536.0) / 256.0);
        float tr = floor(pk - tb * 65536.0 - tg * 256.0);
        o.color.rgb *= float3(tr, tg, tb) / 255.0;
        fadeEnd = 0.0;
        clipStart = 0.0;
    }
    o.light = float4(clipStart, M[3][1], M[3][2], fadeEnd);
    // [3][3] selects the lighting path (see dh_terrain_light.hlsli and LodRenderer).
    o.texCoords = dhLightVarying(M[3][3], i.texCoords);
    o.worldPos = world.xyz;
    return o;
}
