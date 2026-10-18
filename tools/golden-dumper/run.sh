#!/usr/bin/env bash
# Regenerates tests/golden/*.gv.json.   Usage: run.sh [suite...]   (no args = all suites)
# "pos" also regenerates its shards (pos_1 ...). Set SKIP_BUILD=1 to reuse the last build.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"
[ "${SKIP_BUILD:-0}" = 1 ] || "$HERE/build.sh"
CP="$HERE/build/classes:$(cat "$HERE/build/classpath.txt")"
cd "$REPO"
exec "$JAVA_HOME/bin/java" -Dgolden.out="$REPO/tests/golden" -cp "$CP" dumper.Main "$@"
