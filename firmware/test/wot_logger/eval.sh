#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the WOT logger module.
#
# Claude Code MUST run this and exit 0 before declaring Prompt 2 done.
# If any check fails, fix the issue and re-run. Do not declare success
# while any check is FAIL.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/wot_logger/eval.sh
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
LOG_DIR="$SRC_ROOT/logger"
CMD_DIR="$SRC_ROOT/commands"
CFG_HEADER="$SRC_ROOT/config/wot_logger_config.h"
TEST_FILE="$FW_ROOT/test/test_wot_logger.c"
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
# Section 1 — File structure (lifecycle / recorder / uploader split)
# ---------------------------------------------------------------------------
section "1. File structure"

REQUIRED_FILES=(
    "$LOG_DIR/wot_logger.h"
    "$LOG_DIR/wot_logger.c"
    "$LOG_DIR/wot_recorder.h"
    "$LOG_DIR/wot_recorder.c"
    "$LOG_DIR/wot_uploader.h"
    "$LOG_DIR/wot_uploader.c"
    "$CFG_HEADER"
    "$CMD_DIR/wot_log_commands.h"
    "$CMD_DIR/wot_log_commands.c"
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
# Section 2 — Public API surface in wot_logger.h
# ---------------------------------------------------------------------------
section "2. Public API surface"

if [ -f "$LOG_DIR/wot_logger.h" ]; then
    H="$LOG_DIR/wot_logger.h"
    for sym in \
        "wot_logger_init" \
        "wot_logger_start" \
        "wot_logger_stop" \
        "wot_logger_is_running" \
        "wot_logger_register_with_feature_manager"
    do
        if grep -q "$sym" "$H"; then
            pass "header declares: $sym"
        else
            fail "header missing symbol: $sym"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 3 — No-magic-numbers scan on each new .c file
# ---------------------------------------------------------------------------
section "3. No magic numbers in new .c files"

scan_magic() {
    local file="$1"
    [ -f "$file" ] || return 0
    # Use perl for the strip pass so multi-line /* ... */ comments
    # (which can carry version numbers, dates, RFC refs, etc. that
    # otherwise leak into the scanner as false-positive magic numbers)
    # are properly removed. The sed fallback we replaced couldn't span
    # newlines; observed 2026-05-22 when P-64 explanatory comments
    # tripped the gate on prose, not code.
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
        || true)
    if [ -z "$suspect" ]; then
        pass "no magic numbers detected in $(basename "$file")"
    else
        fail "$(basename "$file") contains numeric literals that look like magic numbers; move to wot_logger_config.h"
        echo "$suspect" | sed 's/^/        /'
    fi
}

scan_magic "$LOG_DIR/wot_logger.c"
scan_magic "$LOG_DIR/wot_recorder.c"
scan_magic "$LOG_DIR/wot_uploader.c"
scan_magic "$CMD_DIR/wot_log_commands.c"

# ---------------------------------------------------------------------------
# Section 4 — Config header has named #defines + approval annotation
# ---------------------------------------------------------------------------
section "4. wot_logger_config.h uses named #defines"

if [ -f "$CFG_HEADER" ]; then
    DEFINE_COUNT=$(grep -cE '^\s*#define\s+\w+' "$CFG_HEADER" || true)
    if [ "$DEFINE_COUNT" -ge 1 ]; then
        pass "config header declares $DEFINE_COUNT named constant(s)"
    else
        fail "config header declares no #define constants — every tunable value must live here"
    fi
    # Accept either pending-approval phrasing OR a Locked YYYY-MM-DD
    # marker. Every config header must carry an explicit approval-status
    # marker so a reviewer can tell at a glance whether a value is
    # signed off; either form satisfies that.
    if grep -qiE 'needs?\s+(approval|review|sign-off)' "$CFG_HEADER"; then
        pass "config header flags defaults as needing approval"
    elif grep -qE 'Locked\s+[0-9]{4}-[0-9]{2}-[0-9]{2}' "$CFG_HEADER"; then
        pass "config header marked Locked with date stamp"
    else
        fail "config header should carry an approval-status marker — either 'needs approval/review/sign-off' (pre-lock) or 'Locked YYYY-MM-DD' (post-lock)"
    fi
    # Required keys per Sean's directive in the kickoff response.
    for key in \
        "WOT_TRIGGER_VARIABLE_NAME" \
        "WOT_TRIGGER_THRESHOLD_PERCENT" \
        "WOT_TRIGGER_COOLDOWN_MS" \
        "WOT_MAX_RECORD_DURATION_MS" \
        "WOT_UPLOAD_RETRY_INTERVAL_MS" \
        "WOT_UPLOAD_MAX_QUEUE_BYTES" \
        "WOT_UPLOAD_ENDPOINT_PATH"
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

# Frozen modules and Prompt-1 deliverables MUST stay untouched.
# Other command handler files (besides wot_log_commands.{c,h} and the
# commands.c registry, which IS in scope per Sean's response) are
# also forbidden — Prompt 2 is supposed to add WOT-only wiring.
FORBIDDEN=(
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/feature_manager"
    "firmware/src/commands/command_handler.c"
    "firmware/src/commands/command_handler.h"
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
    "firmware/src/flash"
    "firmware/test/feature_manager"
    "firmware/test/test_feature_manager.c"
    "firmware/test/verify_frozen.sh"
)

if command -v git >/dev/null 2>&1 && git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    # Per-prompt overrides via firmware/test/_shared/eval_forbidden_overrides.txt
    # (see its header for format + rationale).
    OVERRIDE_FILE="$PROJECT_ROOT/firmware/test/_shared/eval_forbidden_overrides.txt"
    OVERRIDES=()
    if [ -f "$OVERRIDE_FILE" ]; then
        while IFS= read -r ol; do
            ol="${ol# }"; ol="${ol% }"
            [ -z "$ol" ] && continue
            case "$ol" in '#'*) continue;; esac
            OVERRIDES+=("$ol")
        done < "$OVERRIDE_FILE"
    fi
    is_overridden() {
        local p="$1"
        for ov in "${OVERRIDES[@]}"; do
            [ "$p" = "$ov" ] && return 0
            case "$ov" in */) case "$p" in "$ov"*) return 0;; esac;; esac
        done
        return 1
    }
    for f in "${FORBIDDEN[@]}"; do
        git_lines=$(git -C "$PROJECT_ROOT" status --porcelain "$f" 2>/dev/null)
        if [ -z "$git_lines" ]; then
            pass "untouched: $f"
            continue
        fi
        unauthorized=()
        while IFS= read -r l; do
            [ -z "$l" ] && continue
            rp="${l:3}"
            case "$rp" in *' -> '*) rp="${rp#* -> }";; esac
            if ! is_overridden "$rp"; then
                unauthorized+=("$rp")
            fi
        done <<< "$git_lines"
        if [ ${#unauthorized[@]} -eq 0 ]; then
            pass "untouched (all changes in $f allowlisted): $f"
        else
            fail "forbidden file/dir was modified: $f"
            for u in "${unauthorized[@]}"; do
                echo "        unauthorized: $u"
            done
        fi
    done
else
    echo "  SKIP  git not available — cannot verify forbidden-modifications check"
fi

# Frozen modules check piggybacks on the canonical harness.
if [ -x "$PROJECT_ROOT/firmware/test/verify_frozen.sh" ]; then
    if "$PROJECT_ROOT/firmware/test/verify_frozen.sh" >/dev/null 2>&1; then
        pass "frozen modules unchanged (scal/bdef/ecu_write)"
    else
        fail "frozen modules modified — see firmware/test/verify_frozen.sh output"
    fi
else
    fail "verify_frozen.sh missing or not executable"
fi

# ---------------------------------------------------------------------------
# Section 6 — Host-side unit test compiles + runs
# ---------------------------------------------------------------------------
section "6. Host unit test"

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
# Section 7 — Required test scenarios in test_wot_logger.c
# ---------------------------------------------------------------------------
section "7. Required test scenarios in test_wot_logger.c"

if [ -f "$TEST_FILE" ]; then
    declare -a REQUIRED=(
        "no.*other.*feature"          # start when nothing else active
        "arbitrat"                    # arbitration via feature_manager
        "threshold"                   # trigger crosses threshold
        "(60s|hard.*cap|max.*record)" # 60s elapsed → auto-end
        "(upload.*success|delete.*200)" # upload success → local copy deleted
        "(upload.*5xx|retain.*retry)" # upload 5xx → retain, retry
        "(gzip|0x1[fF])"              # structural gzip check
        "(clock|fast.forward)"        # test-controllable clock
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
# Section 8 — Full firmware build (idf.py)
# ---------------------------------------------------------------------------
section "8. Full firmware build (idf.py)"

if [ "${SKIP_IDF_BUILD:-0}" = "1" ]; then
    echo "  SKIP  SKIP_IDF_BUILD=1 set; skipping idf.py build"
elif [ -d "${IDF_PATH:-$HOME/esp/esp-idf}" ]; then
    if (cd "$FW_ROOT" && ./build.sh > /tmp/futuner_wot_build.log 2>&1); then
        pass "idf.py build exited 0"
    else
        fail "idf.py build FAILED — see /tmp/futuner_wot_build.log"
        tail -40 /tmp/futuner_wot_build.log | sed 's/^/        /'
    fi
else
    echo "  SKIP  IDF_PATH not found; skipping firmware build"
fi

# ---------------------------------------------------------------------------
# Golden response-shape + storage + upload contract
# ---------------------------------------------------------------------------
section "Golden response-shape + storage + upload contract"

GOLDEN_DIR="$SCRIPT_DIR/golden"
if [ ! -d "$GOLDEN_DIR" ]; then
    fail "golden/ directory missing — Phase 1 close-out golden-fixture pinning required"
else
    pass "golden/ directory present"
    if [ -f "$GOLDEN_DIR/wot_log_response.schema.json" ]; then
        pass "wot_log_response.schema.json contract documented"
    else
        fail "wot_log_response.schema.json missing"
    fi

    # wot_log_start / wot_log_stop response shape
    for field in "success" "command" "active_feature"; do
        if grep -qE "\"${field}\"" "$CMD_DIR/wot_log_commands.c" 2>/dev/null; then
            pass "wot_log_{start,stop} response field \"$field\" emitted"
        else
            fail "wot_log_{start,stop} response field \"$field\" missing"
        fi
    done

    # P-65 fix: queue dir constant must point at /cal/wot, not /storage/wot.
    if grep -qE 'WOT_QUEUE_DIR_PATH[[:space:]]+"/cal/wot"' "$CFG_HEADER" 2>/dev/null; then
        pass "WOT_QUEUE_DIR_PATH = /cal/wot (P-65 fix in place)"
    else
        fail "WOT_QUEUE_DIR_PATH not /cal/wot — P-65 regression"
    fi

    # Upload endpoint constant exists
    if grep -qE 'WOT_UPLOAD_ENDPOINT_PATH[[:space:]]+"/api/v1/telemetry/log"' \
           "$CFG_HEADER" 2>/dev/null; then
        pass "WOT_UPLOAD_ENDPOINT_PATH = /api/v1/telemetry/log"
    else
        fail "WOT_UPLOAD_ENDPOINT_PATH constant missing or wrong value"
    fi

    # Registry must contain the wot + logger commands the golden lists.
    for cmd in wot_log_start wot_log_stop get_logger_data get_logger_data_raw logger_start logger_stop; do
        if grep -qE "^\s*\{[[:space:]]*\"${cmd}\"," "$SRC_ROOT/commands/commands.c"; then
            pass "$cmd registered in COMMAND_REGISTRY"
        else
            fail "$cmd NOT registered in COMMAND_REGISTRY"
        fi
    done
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
echo "All checks green. You may declare Prompt 2 complete and hand back to Sean."
exit 0
