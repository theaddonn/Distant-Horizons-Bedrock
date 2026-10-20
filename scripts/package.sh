#!/usr/bin/env bash
# Usage: scripts/package.sh client|server <outdir>
# Assembles DistantHorizons-<client|server>-<version>.zip in <outdir> from bin/ (built DLL), dist/ (prebuilt
# material) and data/. Layout (zip root):
#   client: mods/DistantHorizons/{DistantHorizons.dll, manifest.json, materials/DH_Terrain.material.bin, data/*.json}
#   server: plugins/DistantHorizonsServer/{DistantHorizonsServer.dll, manifest.json, data/*.json}
# plus LICENSE, LICENSE.LESSER, README.md, CHANGELOG.md at the zip root.
# Does not run the game and does not write any config.json (the mod writes it on first start).
# Needs: bash, sha256sum, zip. PDBs are not shipped.
set -euo pipefail
target="${1:-}"; out="${2:-}"
case "$target" in client|server) ;; *) echo "usage: $0 client|server <outdir>" >&2; exit 2;; esac
[ -n "$out" ] || { echo "usage: $0 client|server <outdir>" >&2; exit 2; }
cd "$(dirname "$0")/.."
ROOT="$PWD"
ver=$("$ROOT/scripts/check-version.sh")
if [ "$target" = client ]; then
  name=DistantHorizons; sub=mods
else
  name=DistantHorizonsServer; sub=plugins
fi
src="$ROOT/bin/$name"
if [ ! -f "$src/$name.dll" ] || [ ! -f "$src/manifest.json" ]; then
  echo "no build output in $src (build first)" >&2
  exit 1
fi
mkdir -p "$out"; out=$(cd "$out" && pwd)
stage=$(mktemp -d); trap 'rm -rf "$stage"' EXIT
dst="$stage/$sub/$name"
mkdir -p "$dst/data"
cp "$src/$name.dll" "$src/manifest.json" "$dst/"
cp "$ROOT"/data/*.json "$dst/data/"
if [ "$target" = client ]; then
  if ! (cd "$ROOT/dist/materials" && sha256sum -c DH_Terrain.material.bin.sha256 >&2); then
    echo "dist material does not match its .sha256" >&2
    exit 1
  fi
  mkdir -p "$dst/materials"
  cp "$ROOT/dist/materials/DH_Terrain.material.bin" "$dst/materials/"
fi
zipf="$out/DistantHorizons-$target-$ver.zip"
rm -f "$zipf"
cp "$ROOT/LICENSE" "$ROOT/LICENSE.LESSER" "$ROOT/README.md" "$ROOT/CHANGELOG.md" "$stage/"
(cd "$stage" && find . -type f | sed 's|^\./||' | LC_ALL=C sort | zip -X -q "$zipf" -@)
echo "$zipf"
