#!/usr/bin/env bash
# capture_stop.sh — stop the active candump capture.
#
# Looks up the PID and log path from captures/.lock written by
# capture_start.sh, sends SIGINT (so candump flushes the file cleanly),
# waits for it to exit, then prints the final log path on stdout.
#
# Usage:
#   capture_stop.sh
#
# Exit codes:
#   0  capture stopped cleanly (log path printed on stdout)
#   3  no active capture (lockfile missing or stale)
#   4  failed to stop the capture process within the timeout

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLKIT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

LOCK="$TOOLKIT_DIR/captures/.lock"
if [ ! -f "$LOCK" ]; then
    echo "capture_stop.sh: no active capture (lockfile missing: $LOCK)." >&2
    exit 3
fi

PID="$(awk -F= '$1=="PID"{print $2}' "$LOCK")"
LOG="$(awk -F= '$1=="LOG"{print $2}' "$LOCK")"

if [ -z "$PID" ] || ! kill -0 "$PID" 2>/dev/null; then
    echo "capture_stop.sh: capture not running (stale lockfile, removing)." >&2
    rm -f "$LOCK"
    exit 3
fi

# SIGINT lets candump flush its buffer; SIGTERM/SIGKILL would truncate.
kill -INT "$PID" 2>/dev/null || true

# Wait up to 5s for it to exit.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if ! kill -0 "$PID" 2>/dev/null; then
        break
    fi
    sleep 0.5
done

if kill -0 "$PID" 2>/dev/null; then
    echo "capture_stop.sh: capture (PID $PID) didn't exit within 5s." >&2
    exit 4
fi

rm -f "$LOCK"
echo "$LOG"
