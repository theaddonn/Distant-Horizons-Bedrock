#!/usr/bin/env bash
# NOTE: launches the game window; do not use while someone is working at the display. Prefer manual launch (see scripts/ingame/smoke.sh).
# Launch Minecraft in the background; writes the PID to scripts/.game.pid. Refuses if an instance is running.
set -euo pipefail
source "$(dirname "$0")/common.sh"
if [ -n "$(game_pids)" ]; then echo "game already running: $(game_pids | tr '\n' ' ')" >&2; exit 1; fi
mkdir -p "$ROOT/artifacts"
nohup "$GDK/bin/mc-run.sh" "$BUILD_ID" > "$ROOT/artifacts/game-stdout.log" 2>&1 &
echo $! > "$PID_FILE"
echo "launched launcher pid $(cat "$PID_FILE")"
