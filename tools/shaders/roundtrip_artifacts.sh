#!/usr/bin/env bash
# Unpack (-u) and repack (-r) each given .material.bin with the patched MaterialBinTool and require byte-identical output.
# Usage: tools/shaders/roundtrip_artifacts.sh artifacts/shaders/B2/RenderChunk.material.bin ...
set -euo pipefail
T="${DH_TOOLS:-$HOME/.cache/dh-tools}"
J="${JAVA:-/usr/lib/jvm/java-21-openjdk-amd64/bin/java}"
R="$(mktemp -d)"; trap 'rm -rf "$R"' EXIT
mkdir -p "$R/u" "$R/r"
for f in "$@"; do
  b="$(basename "$f" .material.bin)"
  "$J" -jar "$T/mbt-build/mbt.jar" -u "$f" -o "$R/u" >/dev/null
  "$J" -jar "$T/mbt-build/mbt.jar" -r "$R/u/$b" -o "$R/r" >/dev/null
  if cmp -s "$R/r/$b.material.bin" "$f"; then echo "$b: round-trip identical"; else echo "$b: ROUND-TRIP DIFFERS"; exit 1; fi
done
