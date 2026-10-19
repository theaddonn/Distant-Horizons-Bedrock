// DH_Terrain variant "vanilla VS": the material keeps MeshFallbackPosUVNormalColor's own vertex shader (and its
// fragment uniform table, which this shader does not read); only the fragment stage is ours. Output = vertex colour,
// fog ignored. Used to isolate the vertex-shader matrix convention: if this draws and the full custom build does not,
// dh_terrain.vert.hlsl's transforms are wrong.
struct PSIn {
    float4 pos : SV_Position;
    float4 clipPosition : COLOR3;
    float4 color : COLOR0;
    float4 fog : COLOR2;
    float4 light : COLOR1;
    centroid float2 texCoords : TEXCOORD0;
    float3 worldPos : TEXCOORD3;
};

float4 main(PSIn i) : SV_Target0 { return float4(i.color.rgb, 1.0); }
