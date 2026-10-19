#!/usr/bin/env bash
# Build artifacts/shaders/B3/DH_Terrain.material.bin (offline; never touches the game dir).
#   default       custom VS (dh_terrain.vert.hlsl) + custom FS (dh_terrain.frag.hlsl): Java-style near clip + far fog
#   --vanilla-vs  vanilla MeshFallbackPosUVNormalColor VS + dh_terrain_vanillavs.frag.hlsl (A/B: isolates the VS convention)
# Prereqs: tools/shaders/build_mbt.sh run once; unpacked vanilla materials in $DH_TOOLS/unpacked.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
T="${DH_TOOLS:-$HOME/.cache/dh-tools}"
export LD_LIBRARY_PATH="$T/dxc/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
DXC="$T/dxc/bin/dxc"
SRC="$HERE/src"
W="$REPO/artifacts/shaders/_work"; rm -rf "$W"; mkdir -p "$W"
OUT="$REPO/artifacts/shaders/B3"; mkdir -p "$OUT"
PASSES=AlphaTest,RasterizedTransparent,DepthOnlyAlphaTest,Opaque,RasterizedAlphaTest,Transparent,RasterizedOpaque
VANILLA=0; [ "${1:-}" = "--vanilla-vs" ] && VANILLA=1
COMMON=(--new-material --rename DH_Terrain --unpacked "$T/unpacked/MeshFallbackPosUVNormalColor" --pass "$PASSES"
        --keep-passes "$PASSES" --out "$OUT" --verify --drop-platforms)
if [ $VANILLA = 1 ]; then
  FS="$SRC/dh_terrain_vanillavs.frag.hlsl"
  $DXC -T ps_6_5 -E main "$FS" -Fo "$W/dh.ps65"; $DXC -T ps_6_0 -E main "$FS" -Fo "$W/dh.ps60"
  python3 -I "$HERE/build_material.py" "${COMMON[@]}" \
    --code Direct3D_SM65:Fragment="$W/dh.ps65" --code Direct3D_SM60:Fragment="$W/dh.ps60"
else
  # Two vertex constant-buffer layouts exist in MeshFallbackPosUVNormalColor (2026-10-19):
  #   group A (464 B: u_proj@0 u_view@64 FogControl@128 u_model@144 ...)  -> AlphaTest, DepthOnlyAlphaTest, Opaque, Transparent
  #   rasterized (384 B: FogControl@0 u_viewProj@16 u_model@80 ...)       -> RasterizedTransparent, RasterizedAlphaTest, RasterizedOpaque
  # Each pass gets the VS written for its own layout; the fragment table is DhParams only in every pass.
  $DXC -T vs_6_5 -E main "$SRC/dh_terrain.vert.hlsl" -Fo "$W/dh.vs65"
  $DXC -T vs_6_0 -E main "$SRC/dh_terrain.vert.hlsl" -Fo "$W/dh.vs60"
  $DXC -T vs_6_5 -E main "$SRC/dh_terrain_raster.vert.hlsl" -Fo "$W/dhr.vs65"
  $DXC -T vs_6_0 -E main "$SRC/dh_terrain_raster.vert.hlsl" -Fo "$W/dhr.vs60"
  $DXC -T ps_6_5 -E main "$SRC/dh_terrain.frag.hlsl" -Fo "$W/dh.ps65"
  $DXC -T ps_6_0 -E main "$SRC/dh_terrain.frag.hlsl" -Fo "$W/dh.ps60"
  VU="u_proj:mat4:0:0:4,u_view:mat4:0:64:4,FogControl:vec4:0:128:1,u_model:mat4:4:144:16,FogColor:vec4:0:400:1"
  VU="$VU,SubPixelOffset:vec4:0:416:1,UVAnimation:vec4:0:432:1,ZShiftValue:vec4:0:448:1"
  VR="FogControl:vec4:0:0:1,u_viewProj:mat4:0:16:4,u_model:mat4:4:80:16,FogColor:vec4:0:336:1,UVAnimation:vec4:0:352:1,ZShiftValue:vec4:0:368:1"
  RASTER=()
  for P in RasterizedTransparent RasterizedAlphaTest RasterizedOpaque; do
    RASTER+=(--pass-code "$P:Direct3D_SM65:Vertex=$W/dhr.vs65" --pass-code "$P:Direct3D_SM60:Vertex=$W/dhr.vs60"
             --pass-uniforms "$P:Vertex=$VR" --pass-cbuf "$P:Vertex=384")
  done
  python3 -I "$HERE/build_material.py" "${COMMON[@]}" --add-prop DhParams=1,1,1,1 \
    --code Direct3D_SM65:Vertex="$W/dh.vs65" --code Direct3D_SM65:Fragment="$W/dh.ps65" \
    --code Direct3D_SM60:Vertex="$W/dh.vs60" --code Direct3D_SM60:Fragment="$W/dh.ps60" \
    --uniforms Vertex="$VU" --cbuf Vertex=464 \
    --uniforms Fragment="DhParams:vec4f:0:0:1,s_MatTexture:sampler:1:0:1" --cbuf Fragment=16 "${RASTER[@]}"
fi
sha256sum "$OUT/DH_Terrain.material.bin"
