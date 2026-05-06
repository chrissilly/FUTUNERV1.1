#!/usr/bin/env bash
# capture_start.sh — start a candump capture in the background.
#
# Reads defaults from bench/defaults.cfg. CLI flags override.
#
# Usage:
#   capture_start.sh [--iface=can0] [--name=feature_under_test] [--max-seconds=120]
#
# Behavior:
#   - Writes the log to TOOLKIT_DIR/captures/<UTC_iso>_<name>.candump
#     in candump's `-tz -L` format (epoch timestamps + log format —
#     the same format the parser and fixtures use).
#   - Records the capture's PID and the log path in a lockfile at
#     TOOLKIT_DIR/captures/.lock so capture_stop.sh can find it.
#   - Refuses to start if a capture is already active per the lockfile
#     (one capture at a time per iteration — the rationale is that
#     multi-capture interleaving makes the parsed output ambiguous).
#   - --max-seconds is enforced via `timeout`; the capture process is
#     killed cleanly at that point even if capture_stop.sh is not run.
#
# Exit codes:
#   0  capture started (lockfile written, PID alive)
#   2  invalid arguments
#   3  another capture is already active (see captures/.lock)
#   4  candump not installed (Linux only)
#   5  failed to launch candump

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLKIT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/defaults.cfg"

IFACE="$DEFAULT_IFACE"
NAME="$DEFAULT_CAPTURE_NAME"
MAX_SECONDS="$DEFAULT_MAX_SECONDS"

for arg in "$@"; do
    case "$arg" in
        --iface=*)       IFACE="${arg#--iface=}" ;;
        --name=*)        NAME="${arg#--name=}" ;;
        --max-seconds=*) MAX_SECONDS="${arg#--max-seconds=}" ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "capture_start.sh: unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

# Sanitize the name a little so it can't escape the directory.
NAME="$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9._-' '_')"

if ! command -v candump >/dev/null 2>&1; then
    echo "capture_start.sh: 'candump' not found — install can-utils (Linux only)." >&2
    exit 4
fi

CAPTURES_DIR="$TOOLKIT_DIR/captures"
LOCK="$CAPTURES_DIR/.lock"
mkdir -p "$CAPTURES_DIR"

if [ -f "$LOCK" ]; then
    EXISTING_PID="$(awk -F= '$1=="PID"{print $2}' "$LOCK" 2>/dev/null)"
    if [ -n "$EXISTING_PID" ] && kill -0 "$EXISTING_PID" 2>/dev/null; then
        echo "capture_start.sh: capture already active (PID $EXISTING_PID, see $LOCK)." >&2
        echo "  Run capture_stop.sh first." >&2
        exit 3
    fi
    # Stale lockfile — clean it up.
    rm -f "$LOCK"
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="$CAPTURES_DIR/${TS}_${NAME}.candump"

# Use `timeout` so a forgotten capture cannot grow without bound.
# candump -tz emits epoch timestamps; -L emits the log format (one
# frame per line, parseable by parse_uds.py).
( timeout --signal=INT "$MAX_SECONDS" candump -tz -L "$IFACE" >"$LOG" 2>/dev/null ) &
PID=$!

# Tiny grace period so we can verify the child actually started.
sleep 0.2 2>/dev/null || true
if ! kill -0 "$PID" 2>/dev/null; then
    echo "capture_start.sh: candump failed to launch (no PID)." >&2
    rm -f "$LOG"
    exit 5
fi

cat > "$LOCK" <<EOF
PID=$PID
LOG=$LOG
IFACE=$IFACE
NAME=$NAME
STARTED_AT=$TS
EOF

echo "capture_start.sh: capture started (PID $PID), logging to $LOG."
