// Spike B3: DH_Terrain vertex shader. Material cloned from MeshFallbackPosUVNormalColor / Opaque; keeps that pass's
// VS inputs (position, normal, color, texCoords) and its VS->FS varying signature:
//   SV_Position, COLOR3 clipPosition, COLOR0 color, COLOR2 fog, COLOR1 light, TEXCOORD0 texCoords (centroid), TEXCOORD3 worldPos
// Constant buffer layout == vanilla MeshFallback group A (cbuf 464 B): u_proj@0 u_view@64 FogControl@128 u_model[4]@144
// FogColor@400 SubPixelOffset@416 UVAnimation@432 ZShiftValue@448 (only u_proj/u_view/u_model are read).
// u_model[0] smuggling test: frac(u_model[0][3] / 64) is passed to the FS in `light.xyz`.
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
    centroid float2 texCoords : TEXCOORD0;
    float3 worldPos : TEXCOORD3;
};

VSOut main(VSIn i) {
    VSOut o;
    float4 world = mul(u_model[0], float4(i.position, 1.0));
    float4 clip = mul(u_proj, mul(u_view, world));
    o.pos = clip;
    o.clipPosition = clip;
    o.color = i.color;
    o.fog = float4(0, 0, 0, 0);
    o.light = float4(frac(u_model[0][0][3] / 64.0), frac(u_model[0][1][3] / 64.0), frac(u_model[0][2][3] / 64.0), 1.0);
    o.texCoords = i.texCoords;
    o.worldPos = world.xyz;
    return o;
}
