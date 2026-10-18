# Shared paths for the in-game harness scripts (sourced).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GDK="${GDK:-$HOME/.steam/steam/steamapps/common/MinecraftBedrockGDK}"
BUILD_ID="${BUILD_ID:-Retail-1.26.5101-Levi}"
MODS_DIR="$GDK/builds/$BUILD_ID/mods/DistantHorizons"
DH_LOG="$MODS_DIR/logs/dh.log"
export DISPLAY="${DISPLAY:-:1}"
PID_FILE="$ROOT/scripts/.game.pid"
SHOT_DIR="$ROOT/artifacts/screenshots"
game_pids() { ps -eo pid,args | grep 'Minecraft.Windows.exe$' | grep -v grep | awk '{print $1}'; }
