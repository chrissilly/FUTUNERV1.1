#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the DTC read/clear feature.
#
# Claude Code MUST run this and exit 0 before declaring this prompt
# done. If any check fails, fix the issue and re-run. Do not declare
# success while any check is FAIL.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/dtc/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.
#
# Env knobs:
#   SKIP_IDF_BUILD=1   skip the full idf.py build step (CI without IDF)
#   VERBOSE=1          print every check as it runs

set -u

# ---------------------------------------------------------------------------
# Locate the project root regardless of cwd.
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FW_ROOT="$PROJECT_ROOT/firmware"
SRC_ROOT="$FW_ROOT/src"
DTC_DIR="$SRC_ROOT/dtc"
CMD_DIR="$SRC_ROOT/commands"
CFG_HEADER="$SRC_ROOT/config/dtc_config.h"
TEST_FILE="$FW_ROOT/test/test_dtc.c"
HOST_TEST_DIR="$SCRIPT_DIR"

cd "$PROJECT_ROOT"

# ---------------------------------------------------------------------------
# Pretty output
# ---------------------------------------------------------------------------
PASS_COUNT=0
FAIL_COUNT=0
FAILURES=()

pass() {
    PASS_COUNT=$((PASS_COUNT+1))
    [ "${VERBOSE:-0}" = "1" ] && echo "  PASS  $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT+1))
    FAILURES+=("$1")
    echo "  FAIL  $1"
}

section() {
    echo
    echo "=============================================================="
    echo "  $1"
    echo "=============================================================="
}

# ---------------------------------------------------------------------------
# Section 1 — File structure (8 files)
# ---------------------------------------------------------------------------
section "1. File structure"

REQUIRED_FILES=(
    "$DTC_DIR/dtc.h"
    "$DTC_DIR/dtc_uds.c"
    "$DTC_DIR/dtc_feature.c"
    "$CFG_HEADER"
    "$CMD_DIR/dtc_commands.h"
    "$CMD_DIR/dtc_commands.c"
    "$TEST_FILE"
    "$HOST_TEST_DIR/Makefile"
)

for f in "${REQUIRED_FILES[@]}"; do
    if [ -f "$f" ]; then
        pass "exists: ${f#$PROJECT_ROOT/}"
    else
        fail "missing required file: ${f#$PROJECT_ROOT/}"
    fi
done

# ---------------------------------------------------------------------------
# Section 2 — Public API surface in dtc.h
# ---------------------------------------------------------------------------
section "2. Public API surface"

if [ -f "$DTC_DIR/dtc.h" ]; then
    H="$DTC_DIR/dtc.h"
    for sym in \
        "dtc_feature_init" \
        "dtc_register_with_feature_manager" \
        "dtc_read" \
        "dtc_clear"
    do
        if grep -q "$sym" "$H"; then
            pass "header declares: $sym"
        else
            fail "header missing symbol: $sym"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 3 — No-magic-numbers scan on dtc_feature.c and dtc_uds.c
# ---------------------------------------------------------------------------
section "3. No magic numbers in dtc_feature.c and dtc_uds.c"

scan_magic() {
    local file="$1"
    [ -f "$file" ] || return 0
    # Strip C string literals, multi-line /* ... */ block comments, and
    # // line comments BEFORE scanning. Multi-line block comments need a
    # multi-line-aware tool (sed -E processes line-by-line on macOS),
    # so we use perl in slurp (-0777) mode.
    local stripped
    stripped=$(perl -0777 -pe '
        s/"(?:\\.|[^"\\])*"//g;
        s|/\*.*?\*/||gs;
        s|//[^\n]*||g;
    ' "$file")
    local suspect
    suspect=$(printf "%s" "$stripped" \
        | grep -nE '\b[0-9]+\b' \
        | grep -vE '^[^:]+:[^:]*#define\b' \
        | grep -vE '\b[01]\b' \
        | grep -vE '\b(uint|int)(8|16|32|64)_t\b' \
        | grep -vE '0x0+' \
        || true
    )
    if [ -z "$suspect" ]; then
        pass "no magic numbers detected in $(basename "$file")"
    else
        fail "$(basename "$file") contains numeric literals that look like magic numbers; move to dtc_config.h"
        echo "$suspect" | sed 's/^/        /'
    fi
}

scan_magic "$DTC_DIR/dtc_feature.c"
scan_magic "$DTC_DIR/dtc_uds.c"

# ---------------------------------------------------------------------------
# Section 4 — dtc_config.h has named #defines + approval annotation
# ---------------------------------------------------------------------------
section "4. dtc_config.h uses named #defines"

if [ -f "$CFG_HEADER" ]; then
    DEFINE_COUNT=$(grep -cE '^\s*#define\s+\w+' "$CFG_HEADER" || true)
    if [ "$DEFINE_COUNT" -ge 1 ]; then
        pass "config header declares $DEFINE_COUNT named constant(s)"
    else
        fail "config header declares no #define constants — every tunable value must live here"
    fi
    if grep -qiE 'needs?\s+(approval|review|sign-off)' "$CFG_HEADER"; then
        pass "config header flags defaults as needing approval"
    else
        fail "config header should annotate proposed defaults as 'NEEDS SEAN'S APPROVAL'"
    fi
    # Required keys per the kickoff prompt.
    for key in \
        "DTC_READ_TIMEOUT_MS" \
        "DTC_CLEAR_TIMEOUT_MS" \
        "DTC_MAX_CODES_PER_RESPONSE" \
        "DTC_DEFAULT_STATUS_MASK" \
        "DTC_FAMILY_DEFAULT"
    do
        if grep -qE "^\s*#define\s+$key\b" "$CFG_HEADER"; then
            pass "config defines $key"
        else
            fail "config header missing required constant: $key"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 5 — Forbidden modifications check
# ---------------------------------------------------------------------------
section "5. Forbidden modifications check"

# Frozen modules and predecessor-prompt deliverables MUST stay
# untouched. Within commands/, only commands.c, dtc_commands.c, and
# dtc_commands.h are in scope.
FORBIDDEN=(
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/flash"
    "firmware/src/feature_manager/feature_manager.c"
    "firmware/src/logger"
    "firmware/src/commands/command_handler.c"
    "firmware/src/commands/command_handler.h"
    "firmware/src/commands/wot_log_commands.c"
    "firmware/src/commands/wot_log_commands.h"
    "firmware/src/commands/ecu_commands.c"
    "firmware/src/commands/ecu_commands.h"
    "firmware/src/commands/ecu_write_commands.c"
    "firmware/src/commands/logger_commands.c"
    "firmware/src/commands/logger_data_commands.c"
    "firmware/src/commands/system_commands.c"
    "firmware/src/commands/file_commands.c"
    "firmware/src/commands/profile_commands.c"
    "firmware/src/commands/flex_commands.c"
    "firmware/src/commands/can_sniffer.c"
    "firmware/src/commands/flash_commands.c"
    "firmware/test/feature_manager"
    "firmware/test/test_feature_manager.c"
    "firmware/test/wot_logger"
    "firmware/test/verify_frozen.sh"
)

if command -v git >/dev/null 2>&1 && git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    for f in "${FORBIDDEN[@]}"; do
        if git -C "$PROJECT_ROOT" status --porcelain "$f" 2>/dev/null | grep -q '.'; then
            fail "forbidden file/dir was modified: $f"
        else
            pass "untouched: $f"
        fi
    done
else
    echo "  SKIP  git not available — frozen-modules cross-check (Section 6) is the load-bearing guard for scal/bdef/ecu_write integrity"
fi

# ---------------------------------------------------------------------------
# Section 6 — Frozen modules cross-check
# ---------------------------------------------------------------------------
section "6. Frozen modules check (verify_frozen.sh)"

if [ -x "$PROJECT_ROOT/firmware/test/verify_frozen.sh" ]; then
    if "$PROJECT_ROOT/firmware/test/verify_frozen.sh" >/dev/null 2>&1; then
        pass "frozen modules unchanged (scal/bdef/ecu_write)"
    else
        fail "frozen modules modified — re-run firmware/test/verify_frozen.sh for details"
    fi
else
    fail "verify_frozen.sh missing or not executable"
fi

# ---------------------------------------------------------------------------
# Section 7 — Host-side unit test compiles + runs
# ---------------------------------------------------------------------------
section "7. Host unit test"

if [ -f "$HOST_TEST_DIR/Makefile" ]; then
    if (cd "$HOST_TEST_DIR" && make -s clean && make -s); then
        pass "host test compiled"
    else
        fail "host test failed to compile"
    fi

    if [ -x "$HOST_TEST_DIR/host_test_runner" ]; then
        if "$HOST_TEST_DIR/host_test_runner"; then
            pass "host test runner exited 0 (all unit tests pass)"
        else
            fail "host test runner reported failures"
        fi
    else
        fail "host_test_runner binary not found after build"
    fi
else
    fail "no host-test Makefile at $HOST_TEST_DIR/Makefile"
fi

# ---------------------------------------------------------------------------
# Section 8 — Required test scenarios in test_dtc.c
# ---------------------------------------------------------------------------
section "8. Required test scenarios in test_dtc.c"

if [ -f "$TEST_FILE" ]; then
    declare -a REQUIRED=(
        "(read.*positive|positive.*read|read.*two)"        # read positive
        "(NRC|negative)"                                    # read negative
        "multi.*frame"                                      # multi-frame
        "clear"                                             # clear (positive)
        "arbitrat"                                          # arbitration with mock feature
        "(active.*during|manager.*mutex|FEATURE_DTC.*during)" # active during exchange
        "idempotent"                                        # idempotent re-start / re-register
    )
    for pat in "${REQUIRED[@]}"; do
        if grep -iEq "$pat" "$TEST_FILE"; then
            pass "test file references scenario: $pat"
        else
            fail "test file missing scenario: $pat"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 9 — Full firmware build (idf.py) — optional, gated
# ---------------------------------------------------------------------------
section "9. Full firmware build (idf.py)"

if [ "${SKIP_IDF_BUILD:-0}" = "1" ]; then
    echo "  SKIP  SKIP_IDF_BUILD=1 set; skipping idf.py build"
elif [ -d "${IDF_PATH:-$HOME/esp/esp-idf}" ]; then
    if (cd "$FW_ROOT" && ./build.sh > /tmp/futuner_dtc_build.log 2>&1); then
        pass "idf.py build exited 0"
    else
        fail "idf.py build FAILED — see /tmp/futuner_dtc_build.log"
        tail -40 /tmp/futuner_dtc_build.log | sed 's/^/        /'
    fi
else
    echo "  SKIP  IDF_PATH not found; skipping firmware build"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "=============================================================="
echo "  Summary"
echo "=============================================================="
echo "  Passed: $PASS_COUNT"
echo "  Failed: $FAIL_COUNT"
echo

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo "FAILURES:"
    for f in "${FAILURES[@]}"; do
        echo "  - $f"
    done
    echo
    echo "RESULT: FAIL"
    echo "Fix the issues above and re-run. Do not declare done while any check is FAIL."
    exit 1
fi

echo "RESULT: PASS"
echo "All checks green. You may declare the DTC prompt complete and hand back to Sean."
exit 0
