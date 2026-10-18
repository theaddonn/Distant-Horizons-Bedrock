#!/usr/bin/env bash
# Unpack + repack every *.material.bin in a directory and cmp with the originals (read-only on the input).
# Usage: roundtrip_check.sh MATERIALS_DIR [WORKDIR]
set -u
M="$1"; W="${2:-$HOME/.cache/dh-tools/rt}"; J="${JAVA:-/usr/lib/jvm/java-21-openjdk-amd64/bin/java}"
JAR="${MBT_JAR:-$HOME/.cache/dh-tools/mbt-build/mbt.jar}"
rm -rf "$W"; mkdir -p "$W/un" "$W/re"
"$J" -jar "$JAR" -u "$M" -o "$W/un" >/dev/null 2>&1
same=0; diff=0
for d in "$W"/un/*/; do n=$(basename "$d")
  "$J" -jar "$JAR" -r "$d" -o "$W/re" >/dev/null 2>&1
  if cmp -s "$W/re/$n.material.bin" "$M/$n.material.bin"; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $n"; fi
done
echo "identical=$same different=$diff"
