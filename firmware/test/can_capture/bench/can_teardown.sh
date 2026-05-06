#!/usr/bin/env bash
# can_teardown.sh — bring the SocketCAN interface down cleanly.
#
# Reads defaults from bench/defaults.cfg. CLI flags override.
#
# Usage:
#   can_teardown.sh [--iface=can0]
#
# Behavior:
#   - If the interface is already down (or doesn't exist), exits 0.
#   - Otherwise sets link state to down via `ip link`.
#
# Exit codes:
#   0  success (interface down, or not present)
#   2  invalid arguments
#   4  not on Linux / SocketCAN unavailable
#   5  privilege error (need root for `ip link set`)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/defaults.cfg"

IFACE="$DEFAULT_IFACE"

for arg in "$@"; do
    case "$arg" in
        --iface=*) IFACE="${arg#--iface=}" ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "can_teardown.sh: unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

if ! command -v ip >/dev/null 2>&1; then
    echo "can_teardown.sh: 'ip' not found — SocketCAN teardown is Linux-only." >&2
    exit 4
fi

if ! ip link show "$IFACE" >/dev/null 2>&1; then
    echo "can_teardown.sh: $IFACE not present (no-op)."
    exit 0
fi

CURRENT_STATE="$(ip link show "$IFACE" 2>/dev/null | head -1 | awk '{print $9}')"
if [ "$CURRENT_STATE" = "DOWN" ]; then
    echo "can_teardown.sh: $IFACE already down (no-op)."
    exit 0
fi

if ! sudo -n true 2>/dev/null && [ "$(id -u)" -ne 0 ]; then
    echo "can_teardown.sh: needs root (or passwordless sudo) to set link state." >&2
    exit 5
fi

sudo ip link set "$IFACE" down
echo "can_teardown.sh: $IFACE down."
