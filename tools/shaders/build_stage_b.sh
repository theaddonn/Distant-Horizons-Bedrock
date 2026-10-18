#!/usr/bin/env bash
# Build the Stage-B spike artifacts (offline; never touches the game dir).
#   artifacts/shaders/B2/RenderChunk.material.bin   vanilla RenderChunk with a pink Opaque fragment shader
#   artifacts/shaders/B3/DH_Terrain.material.bin    custom material cloned from MeshFallbackPosUVNormalColor / Opaque
# Prereqs: tools/shaders/build_mbt.sh run once; unpacked vanilla materials in $DH_TOOLS/unpacked (see shader-toolchain.md).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
T="${DH_TOOLS:-$HOME/.cache/dh-tools}"
export LD_LIBRARY_PATH="$T/dxc/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
DXC="$T/dxc/bin/dxc"
FXC="$T/hlsl-test/fxc_lite.exe"
export WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/amtool/wineprefix}"
export WINEDEBUG=-all
SRC="$HERE/src"
W="$REPO/artifacts/shaders/_work"; rm -rf "$W"; mkdir -p "$W"
B2="$REPO/artifacts/shaders/B2"; B3="$REPO/artifacts/shaders/B3"; mkdir -p "$B2" "$B3"

fxc() { wine "$FXC" "$1" "$2" "$3" "$4" >/dev/null 2>"$W/fxc.err" || { echo "fxc_lite failed: $*"; cat "$W/fxc.err"; return 1; }; }

# ---- B2: tint ------------------------------------------------------------------------------------------------
$DXC -T ps_6_5 -E main "$SRC/renderchunk_tint.frag.hlsl" -Fo "$W/tint.ps65"
$DXC -T ps_6_0 -E main "$SRC/renderchunk_tint.frag.hlsl" -Fo "$W/tint.ps60"
fxc "$SRC/renderchunk_tint.frag.hlsl" main ps_5_0 "$W/tint.ps50"
fxc "$SRC/renderchunk_tint.frag.hlsl" main ps_4_0 "$W/tint.ps40"
python3 -I "$HERE/build_material.py" --tint-only --unpacked "$T/unpacked/RenderChunk" --pass Opaque --out "$B2" --verify \
  --code Direct3D_SM65:Fragment="$W/tint.ps65" --code Direct3D_SM60:Fragment="$W/tint.ps60" \
  --code Direct3D_SM50:Fragment="$W/tint.ps50" --code Direct3D_SM40:Fragment="$W/tint.ps40"

# ---- B3: DH_Terrain ------------------------------------------------------------------------------------------
$DXC -T vs_6_5 -E main "$SRC/dh_terrain.vert.hlsl" -Fo "$W/dh.vs65"
$DXC -T vs_6_0 -E main "$SRC/dh_terrain.vert.hlsl" -Fo "$W/dh.vs60"
$DXC -T ps_6_5 -E main "$SRC/dh_terrain.frag.hlsl" -Fo "$W/dh.ps65"
$DXC -T ps_6_0 -E main "$SRC/dh_terrain.frag.hlsl" -Fo "$W/dh.ps60"
# Vertex table = vanilla MeshFallback Opaque group A (bytes), Fragment table = DhParams only. SM50/SM40 dropped (D3D12 only).
VU="u_proj:mat4:0:0:4,u_view:mat4:0:64:4,FogControl:vec4:0:128:1,u_model:mat4:4:144:16,FogColor:vec4:0:400:1"
VU="$VU,SubPixelOffset:vec4:0:416:1,UVAnimation:vec4:0:432:1,ZShiftValue:vec4:0:448:1"
python3 -I "$HERE/build_material.py" --new-material --rename DH_Terrain --unpacked "$T/unpacked/MeshFallbackPosUVNormalColor" \
  --pass Opaque --out "$B3" --verify --drop-platforms --add-prop DhParams=1,1,1,1 \
  --code Direct3D_SM65:Vertex="$W/dh.vs65" --code Direct3D_SM65:Fragment="$W/dh.ps65" \
  --code Direct3D_SM60:Vertex="$W/dh.vs60" --code Direct3D_SM60:Fragment="$W/dh.ps60" \
  --uniforms Vertex="$VU" --cbuf Vertex=464 \
  --uniforms Fragment="DhParams:vec4f:0:0:1" --cbuf Fragment=16
