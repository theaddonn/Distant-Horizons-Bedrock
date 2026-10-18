#!/usr/bin/env bash
# Usage: scripts/ingame-test.sh <scenario>
# User-driven checklist runner: runs scripts/ingame/<scenario>.sh, which deploys the DLL, tells the user which manual
# steps to perform (launch the game, enter a world, press keys), waits for log markers in dh.log and takes the
# screenshot once the user confirms. It never launches or kills the game. Exit 0 pass / 1 fail.
D="$(cd "$(dirname "$0")" && pwd)"
s="${1:?usage: ingame-test.sh <scenario>}"
[ -f "$D/ingame/$s.sh" ] || { echo "no scenario $s (have: $(ls "$D/ingame" | sed 's/\.sh$//' | tr '\n' ' '))" >&2; exit 2; }
bash "$D/ingame/$s.sh"
rc=$?
[ $rc -eq 0 ] && echo "SCENARIO $s: PASS" || echo "SCENARIO $s: FAIL"
exit $rc
