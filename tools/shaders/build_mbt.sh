#!/usr/bin/env bash
# Build ddf8196/MaterialBinTool (v0.9.1, commit 508dba7) with the local v26 patch, WITHOUT gradle
# (its bundled gradle 6.7.1 cannot run on JDK 21+). Output: $DH_TOOLS/mbt-build/mbt.jar
# Usage: tools/shaders/build_mbt.sh   (DH_TOOLS defaults to ~/.cache/dh-tools)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
T="${DH_TOOLS:-$HOME/.cache/dh-tools}"
JH="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"
mkdir -p "$T/libs"; cd "$T"
[ -d MaterialBinTool ] || git clone -q https://github.com/ddf8196/MaterialBinTool
cd MaterialBinTool
git checkout -q 508dba7
git apply --check "$HERE/matbintool-v26.patch" 2>/dev/null && git apply "$HERE/matbintool-v26.patch" || echo "patch already applied / not applicable, continuing"
cd "$T/libs"
for u in com/google/code/gson/gson/2.9.1/gson-2.9.1.jar com/beust/jcommander/1.82/jcommander-1.82.jar \
         com/google/code/findbugs/jsr305/3.0.2/jsr305-3.0.2.jar; do
  [ -f "$(basename $u)" ] || curl -sLO "https://repo1.maven.org/maven2/$u"
done
cd "$T"; B=mbt-build; rm -rf $B; mkdir -p $B/classes
find MaterialBinTool/src/main/java MaterialBinTool/thirdparty/*/src/main/java -name '*.java' > $B/srcs.txt
"$JH/bin/javac" -nowarn -d $B/classes -cp "libs/gson-2.9.1.jar:libs/jcommander-1.82.jar:libs/jsr305-3.0.2.jar" @$B/srcs.txt
cp -r MaterialBinTool/src/main/resources/* $B/classes/
printf 'Main-Class: com.ddf.materialbintool.main.Main\nClass-Path: ../libs/gson-2.9.1.jar ../libs/jcommander-1.82.jar ../libs/jsr305-3.0.2.jar\n' > $B/m.txt
(cd $B/classes && "$JH/bin/jar" cfm ../mbt.jar ../m.txt .)
echo "built $T/$B/mbt.jar   (run: java -jar $T/$B/mbt.jar -u X.material.bin -o OUT | -r OUTDIR/X -o OUT)"
