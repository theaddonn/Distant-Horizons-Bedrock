#!/usr/bin/env bash
# smoke (manual checklist): deploy -> user launches the game and enters a world -> markers `DH loaded` and
# `level: join` appear in dh.log -> user confirms the HUD is visible -> screenshot.
D="$(cd "$(dirname "$0")/.." && pwd)"
source "$D/common.sh"
"$D/deploy.sh" client || exit 1
mkdir -p "$(dirname "$DH_LOG")"
echo
echo "MANUAL STEP: launch the game via"
echo "    $GDK/bin/mc-run.sh $BUILD_ID"
echo "then enter a world (the build auto-resumes the last world on this machine). Waiting for log markers..."
"$D/wait-for-log.sh" 'DH loaded' 300 || exit 1
"$D/wait-for-log.sh" 'level: join' 600 || exit 1
echo
echo "MANUAL STEP: look at the top-left of the game window: the HUD should show 'DH 0.1.0 (<sha>)', 'frame ms: ...'."
read -r -p "Press Enter here when the HUD is visible (screenshot is taken then; the game stays running)... " _
"$D/screenshot.sh" smoke
echo "--- last dh.log lines ---"
tail -n 8 "$DH_LOG"
