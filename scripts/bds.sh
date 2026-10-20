#!/usr/bin/env bash
# Local Bedrock Dedicated Server (BDS) + LeviLamina test environment under wine, headless.
#   scripts/bds.sh setup        download/verify + unpack BDS and LeviLamina server deps, install the mod (idempotent)
#   scripts/bds.sh install-mod  only refresh the mod (bin/DistantHorizonsServer + data/*.json)
#   scripts/bds.sh start        run the server headless (console from a FIFO, log in console.log)
#   scripts/bds.sh stop         send "stop" to the console, wait, kill if needed
#   scripts/bds.sh cmd <text>   send one console command (e.g. "dh status")
#   scripts/bds.sh log [-f]     show console.log + the mod's logs/dh.log (-f: follow)
#   scripts/bds.sh status
# Env: BDS_ROOT (default ~/.cache/dh-bds/<BDS_VERSION>), BDS_VERSION, BDS_ONLINE_MODE (default true).
# Everything is written under BDS_ROOT / the download cache; never into a game install.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BDS_VERSION="${BDS_VERSION:-1.26.51.1}"
CACHE="${DH_BDS_CACHE:-$HOME/.cache/dh-bds}"
BDS_ROOT="${BDS_ROOT:-$CACHE/$BDS_VERSION}"
DL="$CACHE/downloads"
MOD_NAME=DistantHorizonsServer
MOD_DIR="$BDS_ROOT/plugins/$MOD_NAME"
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"

# name|relative file|url|expected sha256 (empty = trust-on-first-use, hash is printed)
BDS_URL="https://www.minecraft.net/bedrockdedicatedserver/bin-win/bedrock-server-$BDS_VERSION.zip"
GH=https://github.com/LiteLDev
ASSETS=(
  "bds|bedrock-server-$BDS_VERSION-win.zip|$BDS_URL|a1c1ebefde3036223234179f7c1eab4b445b4727f0482a0554818c9bcaf8e4a5"
  "levilamina|levilamina-v26.51.6-server-release-windows-x64.zip|$GH/LeviLamina/releases/download/v26.51.6/levilamina-v26.51.6-server-release-windows-x64.zip|d2277ac4ec042e3498f00160ce1aa0e2dcec23f2b9a902925ac9b27a3f0feb40"
  "preloader|preloader-v1.16.3-windows-x64.zip|$GH/PreLoader/releases/download/v1.16.3/preloader-v1.16.3-windows-x64.zip|31dc9419d4a8c1c382d6c2629243757a28141c878b636d17b92b1b2fd0c3af1a"
  "rtdata|bedrock-runtime-data-v26.51.1-server.7-windows-x64.zip|$GH/bedrock-runtime-data/releases/download/v26.51.1-server.7/bedrock-runtime-data-v26.51.1-server.7-windows-x64.zip|44e548f693b2eb1bd5ccd8b56169cdbf2b9f01e7d1833717c6351bd8f7a2e385"
  "peeditor|peeditor-v3.9.2-windows-x64.zip|$GH/PeEditor/releases/download/v3.9.2/peeditor-windows-x64.zip|cfeba1e9c40fc65a1310853d5cd3d5431d7bc3855c68e23b3d9c7a26f1fe42c2"
)

PID_FILE="$BDS_ROOT/server.pid"
HOLD_FILE="$BDS_ROOT/fifo-holder.pid"
FIFO="$BDS_ROOT/console.in"
CLOG="$BDS_ROOT/console.log"
export WINEPREFIX="$BDS_ROOT/wineprefix" WINEDEBUG=-all WINEARCH=win64

die() { echo "bds.sh: $*" >&2; exit 1; }
alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

fetch() { # name file url sha
  local d="$DL/$1" f="$DL/$1/$2" got
  mkdir -p "$d"
  if [ ! -s "$f" ]; then
    echo "downloading $3"
    curl -fsSL -A "$UA" -o "$f.part" "$3"; mv "$f.part" "$f"
  fi
  got=$(sha256sum "$f" | cut -d' ' -f1)
  echo "sha256 $got  $2"
  [ -z "$4" ] || [ "$got" = "$4" ] || die "sha256 mismatch for $2 (expected $4)"
}

install_mod() {
  local src="$ROOT/bin/$MOD_NAME"
  [ -f "$src/$MOD_NAME.dll" ] || die "no build output in $src (run scripts/build.sh server)"
  [ -d "$BDS_ROOT/plugins" ] || die "run '$0 setup' first"
  mkdir -p "$MOD_DIR/data"
  cp -f "$src/$MOD_NAME.dll" "$src/manifest.json" "$MOD_DIR/"
  [ ! -f "$src/$MOD_NAME.pdb" ] || cp -f "$src/$MOD_NAME.pdb" "$MOD_DIR/"
  cp -f "$ROOT"/data/block_table.json "$ROOT"/data/biome_table.json "$MOD_DIR/data/"
  echo "installed $MOD_NAME.dll sha256=$(sha256sum "$MOD_DIR/$MOD_NAME.dll" | cut -d' ' -f1) -> $MOD_DIR"
}

write_props() {
  local p="$BDS_ROOT/server.properties" online="${BDS_ONLINE_MODE:-true}"
  cat > "$p" <<EOF
server-name=DH Test Server
transport=nethernet
gamemode=creative
difficulty=peaceful
allow-cheats=true
max-players=4
online-mode=$online
allow-list=false
server-port=19132
server-portv6=19133
enable-lan-visibility=true
level-name=dhtest
level-seed=12345
default-player-permission-level=operator
view-distance=12
tick-distance=4
content-log-file-enabled=false
EOF
}

setup() {
  command -v wine >/dev/null || die "wine not found"
  local a
  for a in "${ASSETS[@]}"; do IFS='|' read -r n f u s <<<"$a"; fetch "$n" "$f" "$u" "$s"; done
  mkdir -p "$BDS_ROOT"
  if [ ! -f "$BDS_ROOT/.bds-unpacked" ]; then
    unzip -qo "$DL/bds/bedrock-server-$BDS_VERSION-win.zip" -d "$BDS_ROOT"
    touch "$BDS_ROOT/.bds-unpacked"
  fi
  # LeviLamina server (tooth.json v26.51.6): LeviLamina/ -> plugins/LeviLamina/, PreLoader.dll + bedrock_runtime_data in the root.
  mkdir -p "$BDS_ROOT/plugins"
  unzip -qo "$DL/levilamina/levilamina-v26.51.6-server-release-windows-x64.zip" 'LeviLamina/*' -d "$BDS_ROOT/plugins"
  unzip -qjo "$DL/preloader/preloader-v1.16.3-windows-x64.zip" bin/PreLoader.dll -d "$BDS_ROOT"
  unzip -qo "$DL/rtdata/bedrock-runtime-data-v26.51.1-server.7-windows-x64.zip" bedrock_runtime_data -d "$BDS_ROOT"
  unzip -qjo "$DL/peeditor/peeditor-v3.9.2-windows-x64.zip" PeEditor.exe -d "$BDS_ROOT"
  # PeEditor -mb: makes bedrock_server_mod.exe (bedrock_server.exe patched to load PreLoader.dll); re-run when bedrock_server.exe changes.
  if [ ! -f "$BDS_ROOT/bedrock_server_mod.exe" ]; then
    rm -f "$BDS_ROOT/bedrock_server_mod.exe"
    ( cd "$BDS_ROOT" && wine PeEditor.exe -mb ) || die "PeEditor failed"
  fi
  [ -f "$BDS_ROOT/bedrock_server_mod.exe" ] || die "PeEditor did not produce bedrock_server_mod.exe"
  [ -f "$BDS_ROOT/server.properties.dh" ] || { write_props; touch "$BDS_ROOT/server.properties.dh"; }
  [ -f "$ROOT/bin/$MOD_NAME/$MOD_NAME.dll" ] && install_mod || echo "mod not built yet (scripts/build.sh server), skipping install-mod"
  echo "setup OK: $BDS_ROOT"
}

start() {
  [ -f "$BDS_ROOT/bedrock_server_mod.exe" ] || die "run '$0 setup' first"
  if alive "$PID_FILE"; then echo "already running (pid $(cat "$PID_FILE"))"; return 0; fi
  [ -p "$FIFO" ] || { rm -f "$FIFO"; mkfifo "$FIFO"; }
  # holder keeps the FIFO open read/write so the server never sees EOF on stdin
  setsid bash -c 'exec 3<>"$1"; exec sleep 2147483647' _ "$FIFO" >/dev/null 2>&1 &
  echo $! > "$HOLD_FILE"
  : >> "$CLOG"; echo "=== start $(date -Is) ===" >> "$CLOG"
  cd "$BDS_ROOT"
  setsid nohup bash -c 'exec wine bedrock_server_mod.exe < "$1" >> "$2" 2>&1' _ "$FIFO" "$CLOG" >/dev/null 2>&1 &
  echo $! > "$PID_FILE"
  echo "started (pid $(cat "$PID_FILE")); console: $CLOG"
}

cmd() {
  alive "$PID_FILE" || die "server not running"
  timeout 5 bash -c 'printf "%s\n" "$2" > "$1"' _ "$FIFO" "$*"
}

stop() {
  if alive "$PID_FILE"; then
    timeout 5 bash -c 'printf "stop\n" > "$1"' _ "$FIFO" || true
    local i
    for i in $(seq 1 40); do alive "$PID_FILE" || break; sleep 1; done
    if alive "$PID_FILE"; then echo "graceful stop timed out, killing"; kill "$(cat "$PID_FILE")" 2>/dev/null || true; sleep 2; fi
  fi
  alive "$HOLD_FILE" && kill "$(cat "$HOLD_FILE")" 2>/dev/null || true
  rm -f "$PID_FILE" "$HOLD_FILE"
  [ -d "$WINEPREFIX" ] && wineserver -k 2>/dev/null || true
  echo "stopped"
}

status() {
  if alive "$PID_FILE"; then echo "running (pid $(cat "$PID_FILE")), root $BDS_ROOT"
  elif pgrep -f 'bedrock_server_mod.exe' >/dev/null; then echo "running (pid file stale; bedrock_server_mod.exe found)"
  else echo "not running"; exit 1; fi
}

logs() {
  local dh="$MOD_DIR/logs/dh.log"
  if [ "${1:-}" = -f ]; then tail -n 50 -F "$CLOG" "$dh" 2>/dev/null; else
    echo "--- $CLOG"; tail -n "${LINES_N:-80}" "$CLOG" 2>/dev/null || true
    echo "--- $dh"; tail -n "${LINES_N:-80}" "$dh" 2>/dev/null || true
  fi
}

case "${1:-}" in
  setup) setup ;;
  install-mod) install_mod ;;
  start) start ;;
  stop) stop ;;
  cmd) shift; cmd "$@" ;;
  log) shift; logs "$@" ;;
  status) status ;;
  *) echo "usage: $0 {setup|install-mod|start|stop|cmd <text>|log [-f]|status}" >&2; exit 2 ;;
esac
