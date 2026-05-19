#!/usr/bin/env bash
# can_setup.sh — bring the SocketCAN interface up at the configured bitrate.
#
# Reads defaults from bench/defaults.cfg. CLI flags override.
#
# Usage:
#   can_setup.sh [--iface=can0] [--bitrate=500000]
#
# Behavior:
#   - If the interface is already up at the right bitrate, exits 0 (idempotent).
#   - If the interface is up at a DIFFERENT bitrate, exits 3 — refuses to
#     silently overwrite, since that's almost always a misconfiguration.
#   - On macOS (or anywhere `ip link` is missing), exits 4 with a
#     "Linux only" message; the gs_usb backend is brought up by python-can
#     directly on macOS, not through SocketCAN.
#
# Exit codes:
#   0  success (interface up at the requested bitrate)
#   2  invalid arguments
#   3  interface up at a conflicting bitrate (resolve manually)
#   4  not on Linux / SocketCAN unavailable
#   5  privilege error (need root for `ip link set`)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/defaults.cfg"

IFACE="$DEFAULT_IFACE"
BITRATE="$DEFAULT_BITRATE"

for arg in "$@"; do
    case "$arg" in
        --iface=*)   IFACE="${arg#--iface=}" ;;
        --bitrate=*) BITRATE="${arg#--bitrate=}" ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "can_setup.sh: unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

if ! command -v ip >/dev/null 2>&1; then
    echo "can_setup.sh: 'ip' not found — SocketCAN setup is Linux-only." >&2
    echo "  On macOS, the Candlelight is brought up by python-can directly." >&2
    exit 4
fi

# Check current state.
if ip link show "$IFACE" >/dev/null 2>&1; then
    CURRENT_BITRATE="$(ip -details link show "$IFACE" 2>/dev/null \
        | awk '/bitrate/ {for(i=1;i<=NF;i++) if($i=="bitrate") print $(i+1)}' \
        | head -1)"
    CURRENT_STATE="$(ip link show "$IFACE" 2>/dev/null | head -1 | awk '{print $9}')"

    if [ "$CURRENT_STATE" = "UP" ] && [ "$CURRENT_BITRATE" = "$BITRATE" ]; then
        echo "can_setup.sh: $IFACE already up at ${BITRATE} bps (no-op)."
        exit 0
    fi

    if [ "$CURRENT_STATE" = "UP" ] && [ -n "$CURRENT_BITRATE" ] && [ "$CURRENT_BITRATE" != "$BITRATE" ]; then
        echo "can_setup.sh: $IFACE is up at ${CURRENT_BITRATE} bps, but you asked for ${BITRATE} bps." >&2
        echo "  Refusing to overwrite. Bring it down first:" >&2
        echo "    sudo ip link set $IFACE down" >&2
        exit 3
    fi
fi

# Bring up.
if ! sudo -n true 2>/dev/null && [ "$(id -u)" -ne 0 ]; then
    echo "can_setup.sh: needs root (or passwordless sudo) to set link state." >&2
    exit 5
fi

set -e
sudo ip link set "$IFACE" down 2>/dev/null || true
sudo ip link set "$IFACE" type can bitrate "$BITRATE"
sudo ip link set "$IFACE" up
echo "can_setup.sh: $IFACE up at ${BITRATE} bps."
