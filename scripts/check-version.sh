#!/usr/bin/env bash
# Verify that every place that carries the mod version agrees with xmake.lua (the source of truth).
# Prints the version on success; exits 1 on a mismatch.
set -euo pipefail
cd "$(dirname "$0")/.."
x=$(sed -n 's/^local DH_VERSION = "\([^"]*\)".*/\1/p' xmake.lua)
t=$(python3 -I -c 'import json;print(json.load(open("tooth.json"))["version"])')
h=$(sed -n 's/^#define DH_MOD_VERSION "\([^"]*\)".*/\1/p' src/client/Version.h)
c=$(sed -n 's/.*coreVersion() { return "\([^"]*\)".*/\1/p' src/core/dh/Version.cpp)
echo "xmake.lua=$x tooth.json=$t Version.h=$h core/Version.cpp=$c" >&2
if [ -z "$x" ] || [ "$x" != "$t" ] || [ "$x" != "$h" ] || [ "$x" != "$c" ]; then
  echo "version mismatch" >&2
  exit 1
fi
# CHANGELOG must have a heading for this version (or Unreleased).
if ! grep -Eq "^## \[(${x//./\\.}|Unreleased)\]" CHANGELOG.md; then
  echo "CHANGELOG.md has no [$x] or [Unreleased] heading" >&2
  exit 1
fi
echo "$x"
