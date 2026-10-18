#!/usr/bin/env bash
# Cross-build the mod DLL for Windows from Linux: scripts/build.sh client|server
set -euo pipefail
target="${1:-}"
case "$target" in client|server) ;; *) echo "usage: $0 client|server" >&2; exit 2;; esac
cd "$(dirname "$0")/.."
export PATH="$HOME/.local/share/amtool/bin:$HOME/.local/bin:$PATH"
xmake f -P . -p windows -a x64 -m release --target_type="$target" --toolchain=clang-cl --sdk="$HOME/.local/share/amtool/winsysroot" -y
xmake -P . -y
if [ "$target" = server ]; then out=bin/DistantHorizonsServer; else out=bin/DistantHorizons; fi
echo "Build OK. Output: $PWD/$out"
ls -l "$out"
