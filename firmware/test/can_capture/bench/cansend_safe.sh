#!/usr/bin/env bash
# cansend_safe.sh — the only path in the toolkit that can transmit.
#
# Wraps `cansend` with a hard refusal unless BOTH of these flags are
# present as the FIRST TWO arguments, in this exact order:
#
#     --allow-tx --target=bench
#
# There is no --target=car option. If a future use case needs car-bus
# transmission, that's a reviewed protocol-level change, not something
# an autonomous Claude Code session adds. The bench-only restriction
# is the project-wide safety boundary (BENCH_CAN_TOOLKIT.md §"Safety").
#
# Every accepted transmission is appended to tx_log/<timestamp>.tx.log
# for audit (one log file per script invocation; matches the resolution
# you actually need when reconstructing what hit the bus).
#
# Usage:
#   cansend_safe.sh --allow-tx --target=bench <iface> <can_frame>
#
# Examples:
#   cansend_safe.sh --allow-tx --target=bench can0 7E0#0322F19000000000
#
# Exit codes:
#   0  transmitted (and audit-logged)
#   2  refused: missing or wrong opt-in flags
#   3  too few arguments after the flags
#   4  cansend not installed (Linux only)
#   5  cansend itself failed

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLKIT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Hard refusal. The flags must be EXACTLY these, in this order, as the
# first two arguments. Anything else — including --target=car, --target=anything,
# missing flag, or flags-after-args — is rejected.
if [ "$#" -lt 2 ] || [ "$1" != "--allow-tx" ] || [ "$2" != "--target=bench" ]; then
    echo "cansend_safe.sh: REFUSED — transmission requires:" >&2
    echo "    cansend_safe.sh --allow-tx --target=bench <iface> <frame>" >&2
    echo "  as the EXACT first two arguments. There is no --target=car option." >&2
    echo "  See docs/BENCH_CAN_TOOLKIT.md §Safety." >&2
    exit 2
fi

shift 2  # consume --allow-tx --target=bench

if [ "$#" -lt 2 ]; then
    echo "cansend_safe.sh: need <iface> and <frame> after the opt-in flags." >&2
    exit 3
fi

if ! command -v cansend >/dev/null 2>&1; then
    echo "cansend_safe.sh: 'cansend' not found — install can-utils (Linux only)." >&2
    exit 4
fi

# Audit log first, send second. If the send fails, the audit still
# shows what we attempted (and the cansend exit status is preserved).
TX_LOG_DIR="$TOOLKIT_DIR/tx_log"
mkdir -p "$TX_LOG_DIR"
TS="$(date -u +%Y%m%dT%H%M%S.%NZ)"
LOG="$TX_LOG_DIR/${TS}.tx.log"

{
    echo "ts=$TS"
    echo "user=${USER:-unknown}"
    echo "iface=$1"
    echo "frame=$2"
    echo "argv=$*"
} > "$LOG"

if cansend "$@"; then
    echo "result=ok" >> "$LOG"
    exit 0
else
    rc=$?
    echo "result=fail rc=$rc" >> "$LOG"
    echo "cansend_safe.sh: cansend failed with exit code $rc (audit: $LOG)." >&2
    exit 5
fi
