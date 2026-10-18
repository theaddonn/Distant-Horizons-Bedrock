#!/usr/bin/env bash
# Kill the game (all Minecraft.Windows.exe processes plus the launcher).
source "$(dirname "$0")/common.sh"
[ -f "$PID_FILE" ] && kill "$(cat "$PID_FILE")" 2>/dev/null
pkill -f 'Minecraft.Windows.exe$' 2>/dev/null
for _ in $(seq 1 20); do [ -z "$(game_pids)" ] && break; sleep 0.5; done
[ -n "$(game_pids)" ] && pkill -9 -f 'Minecraft.Windows.exe$'
rm -f "$PID_FILE"
echo "game stopped"
exit 0
