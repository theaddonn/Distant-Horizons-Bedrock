#!/usr/bin/env bash
# Usage: scripts/deploy.sh client  -- copy bin/DistantHorizons/* into the game's mods dir (keeps config/, logs/).
#        BDS_DIR=/path/to/bedrock_server scripts/deploy.sh server
#                                  -- copy bin/DistantHorizonsServer/* + data/*.json into <BDS_DIR>/plugins/DistantHorizonsServer.
set -euo pipefail
source "$(dirname "$0")/common.sh"
if [ "${1:-client}" = server ]; then
  : "${BDS_DIR:?set BDS_DIR to the Bedrock Dedicated Server directory (with LeviLamina installed)}"
  SRC="$ROOT/bin/DistantHorizonsServer"
  DST="$BDS_DIR/plugins/DistantHorizonsServer"
  [ -f "$SRC/DistantHorizonsServer.dll" ] || { echo "no build output in $SRC (run scripts/build.sh server)" >&2; exit 1; }
  mkdir -p "$DST/data"
  cp -f "$SRC/DistantHorizonsServer.dll" "$SRC/DistantHorizonsServer.pdb" "$SRC/manifest.json" "$DST/"
  cp -f "$ROOT/data/"*.json "$DST/data/"   # block/biome tables read by the server driver
  a=$(sha256sum "$SRC/DistantHorizonsServer.dll" | cut -d' ' -f1)
  b=$(sha256sum "$DST/DistantHorizonsServer.dll" | cut -d' ' -f1)
  [ "$a" = "$b" ] || { echo "sha256 mismatch" >&2; exit 1; }
  echo "deployed DistantHorizonsServer.dll sha256=$a -> $DST"
  exit 0
fi
GAME_MATERIALS="$GDK/builds/$BUILD_ID/data/renderer/materials"
if [ "${1:-client}" = restore-materials ]; then
  python3 -I "$ROOT/tools/shaders/install_game_materials.py" restore "$GAME_MATERIALS"
  exit 0
fi
[ "${1:-client}" = client ] || { echo "usage: $0 client [--game-materials] | server | restore-materials" >&2; exit 2; }
SRC="$ROOT/bin/DistantHorizons"
[ -f "$SRC/DistantHorizons.dll" ] || { echo "no build output in $SRC" >&2; exit 1; }
mkdir -p "$MODS_DIR"
cp -f "$SRC/DistantHorizons.dll" "$SRC/DistantHorizons.pdb" "$SRC/manifest.json" "$MODS_DIR/"
mkdir -p "$MODS_DIR/data" && cp -f "$ROOT/data/"*.json "$MODS_DIR/data/"   # block/biome tables read by the mod
# Stage-B custom materials. The copy under the mod dir is what MaterialInjector would serve, but the compiled
# material loader reads data/renderer/materials from disk directly (2026-10-19), so until a loader-level hook
# exists `--game-materials` patches the game's index on disk (backup kept; `deploy.sh restore-materials` undoes it).
if ls "$ROOT"/artifacts/shaders/B3/*.material.bin >/dev/null 2>&1; then
  mkdir -p "$MODS_DIR/materials" && cp -f "$ROOT"/artifacts/shaders/B3/*.material.bin "$MODS_DIR/materials/"
  if [ "${2:-}" = --game-materials ]; then
    python3 -I "$ROOT/tools/shaders/install_game_materials.py" install "$GAME_MATERIALS" "$ROOT"/artifacts/shaders/B3/*.material.bin
  fi
fi
a=$(sha256sum "$SRC/DistantHorizons.dll" | cut -d' ' -f1)
b=$(sha256sum "$MODS_DIR/DistantHorizons.dll" | cut -d' ' -f1)
[ "$a" = "$b" ] || { echo "sha256 mismatch" >&2; exit 1; }
echo "deployed DistantHorizons.dll sha256=$a -> $MODS_DIR"
