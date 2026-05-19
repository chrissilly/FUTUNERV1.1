#!/usr/bin/env bash
# verify_frozen.sh
#
# Verifies that the frozen live-tune modules (scal/bdef/ecu_write) have
# not been modified from the FUTV1.0 baseline. Reads the canonical
# hashes from firmware/src/frozen_modules.sha256 and compares them to
# the current files.
#
# Exits 0 if all match. Exits non-zero with a list of mismatches and a
# clear "you must not modify these" message if any have drifted.
#
# This script is invoked standalone (`firmware/test/verify_frozen.sh`)
# AND from inside any feature eval harness, so a Claude Code session
# that modifies a frozen file cannot pass its eval.
#
# See firmware/src/FROZEN_MODULES.md for the rationale and the approval
# ritual that must be followed if a frozen file genuinely needs to
# change.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Script lives at FUTV1.1/firmware/test/verify_frozen.sh — go up two levels
# to reach FUTV1.1/, which is the project root that manifest paths are
# relative to.
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
MANIFEST="$PROJECT_ROOT/firmware/src/frozen_modules.sha256"

cd "$PROJECT_ROOT"

if [ ! -f "$MANIFEST" ]; then
    echo "FAIL  frozen_modules.sha256 manifest is missing at $MANIFEST"
    echo "      The frozen-module check cannot run without it."
    exit 2
fi

# Pick the right hash command for the platform.
if command -v sha256sum >/dev/null 2>&1; then
    HASH_CMD="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    HASH_CMD="shasum -a 256"
else
    echo "FAIL  no sha256sum or shasum found on this system"
    exit 2
fi

FAIL_COUNT=0
PASS_COUNT=0
MISMATCHES=()

# Read each line of the manifest: "<hash>  <path>"
while IFS= read -r line; do
    [ -z "$line" ] && continue
    expected_hash="$(echo "$line" | awk '{print $1}')"
    relpath="$(echo "$line" | awk '{print $2}')"
    # Paths in the manifest are relative to PROJECT_ROOT (FUTV1.1/), and
    # we're cd'd there, so use them as-is.

    if [ ! -f "$relpath" ]; then
        echo "FAIL  frozen file missing: $relpath"
        MISMATCHES+=("missing: $relpath")
        FAIL_COUNT=$((FAIL_COUNT+1))
        continue
    fi

    actual_hash="$($HASH_CMD "$relpath" | awk '{print $1}')"
    if [ "$expected_hash" = "$actual_hash" ]; then
        PASS_COUNT=$((PASS_COUNT+1))
    else
        echo "FAIL  frozen file modified: $relpath"
        echo "      expected: $expected_hash"
        echo "      actual:   $actual_hash"
        MISMATCHES+=("modified: $relpath")
        FAIL_COUNT=$((FAIL_COUNT+1))
    fi
done < "$MANIFEST"

echo
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo "=============================================================="
    echo "FROZEN MODULE CHECK: FAIL"
    echo "=============================================================="
    echo
    echo "$FAIL_COUNT frozen file(s) have been modified or are missing:"
    for m in "${MISMATCHES[@]}"; do
        echo "  - $m"
    done
    echo
    echo "These modules carry FUTV1.0's on-car-validated live-tune"
    echo "implementation and must not be modified without the approval"
    echo "ritual documented in firmware/src/FROZEN_MODULES.md."
    echo
    echo "If you are Claude Code and you reached this state by editing"
    echo "scal/bdef/ecu_write: revert your changes. Build new logic"
    echo "alongside these modules, calling their public API; do not"
    echo "modify their bytes."
    echo
    echo "If a change is genuinely required, follow the four-step"
    echo "approval ritual in FROZEN_MODULES.md. Do not just update"
    echo "frozen_modules.sha256 to make this check pass — that would"
    echo "defeat the purpose of having a frozen list."
    exit 1
fi

echo "FROZEN MODULE CHECK: PASS  ($PASS_COUNT files match baseline)"
exit 0
