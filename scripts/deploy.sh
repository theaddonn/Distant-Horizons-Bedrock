#!/usr/bin/env bash
# Usage: scripts/deploy.sh client  -- copy bin/DistantHorizons/* into the game's mods dir (keeps config/, logs/).
set -euo pipefail
source "$(dirname "$0")/common.sh"
[ "${1:-client}" = client ] || { echo "only 'client' is supported" >&2; exit 2; }
SRC="$ROOT/bin/DistantHorizons"
[ -f "$SRC/DistantHorizons.dll" ] || { echo "no build output in $SRC" >&2; exit 1; }
mkdir -p "$MODS_DIR"
cp -f "$SRC/DistantHorizons.dll" "$SRC/DistantHorizons.pdb" "$SRC/manifest.json" "$MODS_DIR/"
mkdir -p "$MODS_DIR/data" && cp -f "$ROOT/data/"*.json "$MODS_DIR/data/"   # block/biome tables read by the mod
# Stage-B custom materials (served to the game by MaterialInjector; no game-dir writes)
if ls "$ROOT"/artifacts/shaders/B3/*.material.bin >/dev/null 2>&1; then
  mkdir -p "$MODS_DIR/materials" && cp -f "$ROOT"/artifacts/shaders/B3/*.material.bin "$MODS_DIR/materials/"
fi
a=$(sha256sum "$SRC/DistantHorizons.dll" | cut -d' ' -f1)
b=$(sha256sum "$MODS_DIR/DistantHorizons.dll" | cut -d' ' -f1)
[ "$a" = "$b" ] || { echo "sha256 mismatch" >&2; exit 1; }
echo "deployed DistantHorizons.dll sha256=$a -> $MODS_DIR"
