#!/usr/bin/env bash
# Usage: wait-for-log.sh <regex> <timeout_s> [--from-start]
# Polls dh.log for a line matching the (grep -E) regex. By default only lines written after this script started.
# Prints the matching line; exit 0 found / 1 timeout.
source "$(dirname "$0")/common.sh"
re="$1"; timeout="${2:-60}"
start=0
if [ "${3:-}" != "--from-start" ] && [ -f "$DH_LOG" ]; then start=$(wc -c < "$DH_LOG"); fi
end=$((SECONDS + timeout))
while [ $SECONDS -lt $end ]; do
  if [ -f "$DH_LOG" ]; then
    size=$(wc -c < "$DH_LOG")
    [ "$size" -lt "$start" ] && start=0   # rotated/truncated
    line=$(tail -c +$((start + 1)) "$DH_LOG" | grep -aE -m1 -- "$re")
    if [ -n "$line" ]; then echo "$line"; exit 0; fi
  fi
  sleep 0.5
done
echo "timeout waiting for /$re/ (${timeout}s)" >&2
exit 1
