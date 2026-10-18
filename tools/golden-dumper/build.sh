#!/usr/bin/env bash
# Compiles the dumper against the ORIGINAL Distant Horizons core.
# Env: DH_ROOT (default /home/lucy/Projects/Code/distant-horizons), JAVA_HOME (default JDK 21),
#      DH_MC_VER (default 1.20.1, only used to satisfy the DH gradle settings).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DH_ROOT="${DH_ROOT:-/home/lucy/Projects/Code/distant-horizons}"
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"
BUILD="$HERE/build"
mkdir -p "$BUILD/classes"

# 1. Build DH :core (+ :api) and write the runtime classpath.
#    Offline first (fast path); fall back to online for the first-ever build.
GRADLE_ARGS=(-q -I "$HERE/init.gradle" :core:printGoldenClasspath -PmcVer="${DH_MC_VER:-1.20.1}" -PgoldenCpOut="$BUILD/classpath.txt")
(cd "$DH_ROOT" && { ./gradlew --offline "${GRADLE_ARGS[@]}" >/dev/null 2>&1 || ./gradlew "${GRADLE_ARGS[@]}" >/dev/null; })

# 2. Compile the dumper.
CP="$(cat "$BUILD/classpath.txt")"
"$JAVA_HOME/bin/javac" -nowarn -encoding UTF-8 -cp "$CP" -d "$BUILD/classes" "$HERE"/src/main/java/dumper/*.java
echo "built $BUILD/classes"
