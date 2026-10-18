#!/usr/bin/env bash
# Usage: screenshot.sh <name>  -> artifacts/screenshots/<name>-<timestamp>.png (prints the path)
# Captures the game window (the X root is black under XWayland).
source "$(dirname "$0")/common.sh"
mkdir -p "$SHOT_DIR"
out="$SHOT_DIR/${1:-shot}-$(date +%Y%m%d-%H%M%S).png"
win=$(xdotool search --onlyvisible --name '^Minecraft$' | head -1)
if [ -n "$win" ]; then import -window "$win" "$out"; else import -window root "$out"; fi
echo "$out"
