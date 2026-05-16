#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the SBF live-tune
# orchestrator (Prompt 5).
#
# Claude Code MUST run this and exit 0 before declaring this prompt
# done.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/sbf/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.
#
# Env knobs:
#   SKIP_IDF_BUILD=1   skip the full idf.py build step
#   SKIP_PYTEST=1      skip the cloud-side pytest regression
#   VERBOSE=1          print every check as it runs

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FW_ROOT="$PROJECT_ROOT/firmware"
SRC_ROOT="$FW_ROOT/src"
SBF_DIR="$SRC_ROOT/sbf"
CMD_DIR="$SRC_ROOT/commands"
FLEX_DIR="$SRC_ROOT/flex_fuel"
SBF_CFG="$SRC_ROOT/config/sbf_config.h"
TEST_FILE="$FW_ROOT/test/test_sbf_orchestrator.c"
HOST_TEST_DIR="$SCRIPT_DIR"
CLOUD_DIR="$PROJECT_ROOT/cloud"

cd "$PROJECT_ROOT"

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
# Section 1 — File structure
# ---------------------------------------------------------------------------
section "1. File structure"

REQUIRED_FILES=(
    "$SBF_DIR/sbf_orchestrator.h"
    "$SBF_DIR/sbf_orchestrator.c"
    "$SBF_DIR/sbf_loader.h"
    "$SBF_DIR/sbf_loader.c"
    "$SBF_DIR/sbf_applier.h"
    "$SBF_DIR/sbf_applier.c"
    "$SBF_DIR/sbf_downloader.h"
    "$SBF_DIR/sbf_downloader.c"
    "$SBF_DIR/sbf_variants.h"
    "$SBF_DIR/sbf_variants.c"
    "$SBF_CFG"
    "$CMD_DIR/sbf_commands.h"
    "$CMD_DIR/sbf_commands.c"
    "$FLEX_DIR/blend_engine.h"
    "$FLEX_DIR/blend_engine.c"
    "$TEST_FILE"
    "$HOST_TEST_DIR/Makefile"
)
for f in "${REQUIRED_FILES[@]}"; do
    if [ -f "$f" ]; then pass "exists: ${f#$PROJECT_ROOT/}"
    else                  fail "missing required file: ${f#$PROJECT_ROOT/}"
    fi
done

# ---------------------------------------------------------------------------
# Section 2 — Public API surface
# ---------------------------------------------------------------------------
section "2. Public API surface"

if [ -f "$SBF_DIR/sbf_orchestrator.h" ]; then
    H="$SBF_DIR/sbf_orchestrator.h"
    for sym in \
        "sbf_orchestrator_init" \
        "sbf_orchestrator_register_with_feature_manager" \
        "sbf_orchestrator_live_tune_start" \
        "sbf_orchestrator_live_tune_set" \
        "sbf_orchestrator_live_tune_stop" \
        "sbf_orchestrator_live_tune_status" \
        "sbf_orchestrator_drain_one"
    do
        if grep -q "$sym" "$H"; then pass "orchestrator.h declares: $sym"
        else                          fail "orchestrator.h missing: $sym"
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
    local stripped suspect
    stripped=$(perl -0777 -pe '
        s/"(?:\\.|[^"\\])*"//g;
        s|/\*.*?\*/||gs;
        s|//[^\n]*||g;
    ' "$file")
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
        fail "$(basename "$file") contains numeric literals; move to sbf_config.h"
        echo "$suspect" | sed 's/^/        /'
    fi
}

scan_magic "$SBF_DIR/sbf_orchestrator.c"
scan_magic "$SBF_DIR/sbf_loader.c"
scan_magic "$SBF_DIR/sbf_applier.c"
scan_magic "$SBF_DIR/sbf_downloader.c"
scan_magic "$SBF_DIR/sbf_variants.c"
scan_magic "$CMD_DIR/sbf_commands.c"
scan_magic "$FLEX_DIR/blend_engine.c"

# ---------------------------------------------------------------------------
# Section 4 — Config header
# ---------------------------------------------------------------------------
section "4. sbf_config.h"

if [ -f "$SBF_CFG" ]; then
    DEFINE_COUNT=$(grep -cE '^\s*#define\s+\w+' "$SBF_CFG" || true)
    if [ "$DEFINE_COUNT" -ge 1 ]; then
        pass "sbf_config.h declares $DEFINE_COUNT named constant(s)"
    else
        fail "sbf_config.h declares no #define constants"
    fi
    if grep -qiE 'needs?\s+(approval|review|sign-off)' "$SBF_CFG"; then
        pass "sbf_config.h flags defaults as needing approval"
    elif grep -qE 'Locked\s+[0-9]{4}-[0-9]{2}-[0-9]{2}' "$SBF_CFG"; then
        pass "sbf_config.h marked Locked with date stamp"
    else
        fail "sbf_config.h should carry an approval-status marker — either 'needs approval/review/sign-off' (pre-lock) or 'Locked YYYY-MM-DD' (post-lock)"
    fi
    for key in \
        "SBF_CACHE_DIR_PATH" \
        "SBF_DOWNLOAD_PATH" \
        "SBF_APPLY_HARD_CAP_MS" \
        "SBF_PER_WRITE_TIMEOUT_MS" \
        "SBF_WORKER_QUEUE_DEPTH" \
        "SBF_STAGE_MIN" \
        "SBF_STAGE_MAX" \
        "SBF_BLEND_MAP_POINTS"
    do
        if grep -qE "^\s*#define\s+$key\b" "$SBF_CFG"; then
            pass "sbf_config defines $key"
        else
            fail "sbf_config missing required constant: $key"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 5 — Forbidden modifications check
# ---------------------------------------------------------------------------
section "5. Forbidden modifications check"

# Frozen modules (per FROZEN_MODULES.md), prior-prompt deliverables,
# and out-of-scope command handlers must stay untouched. Per Sean's
# Q2 directive, wot_uploader.{c,h} IS in scope (license-gate one-liner).
FORBIDDEN=(
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/flash"
    "firmware/src/feature_manager"
    "firmware/src/license"
    "firmware/src/vin_pairing"
    "firmware/src/dtc"
    "firmware/src/logger/wot_logger.c"
    "firmware/src/logger/wot_logger.h"
    "firmware/src/logger/wot_recorder.c"
    "firmware/src/logger/wot_recorder.h"
    "firmware/src/commands/command_handler.c"
    "firmware/src/commands/command_handler.h"
    "firmware/src/commands/wot_log_commands.c"
    "firmware/src/commands/wot_log_commands.h"
    "firmware/src/commands/dtc_commands.c"
    "firmware/src/commands/dtc_commands.h"
    "firmware/src/commands/vin_pair_commands.c"
    "firmware/src/commands/vin_pair_commands.h"
    "firmware/src/commands/ecu_commands.c"
    "firmware/src/commands/ecu_commands.h"
    "firmware/src/commands/ecu_write_commands.c"
    "firmware/src/commands/logger_commands.c"
    "firmware/src/commands/logger_data_commands.c"
    "firmware/src/commands/system_commands.c"
    "firmware/src/commands/file_commands.c"
    "firmware/src/commands/profile_commands.c"
    "firmware/src/commands/can_sniffer.c"
    "firmware/src/commands/flex_commands.c"
    "firmware/test/feature_manager"
    "firmware/test/test_feature_manager.c"
    "firmware/test/wot_logger"
    "firmware/test/test_wot_logger.c"
    "firmware/test/dtc"
    "firmware/test/test_dtc.c"
    "firmware/test/vin_pairing"
    "firmware/test/test_vin_pairing.c"
    "firmware/test/verify_frozen.sh"
    "cloud/src/main.py"
    "cloud/tests"
)

if command -v git >/dev/null 2>&1 && git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    # Per-prompt overrides — see firmware/test/_shared/eval_forbidden_overrides.txt
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
    echo "  SKIP  git not available — frozen-modules cross-check is the load-bearing guard"
fi

# ---------------------------------------------------------------------------
# Section 6 — Frozen modules cross-check (CRITICAL)
# ---------------------------------------------------------------------------
section "6. Frozen modules check (verify_frozen.sh)"

if [ -x "$PROJECT_ROOT/firmware/test/verify_frozen.sh" ]; then
    if "$PROJECT_ROOT/firmware/test/verify_frozen.sh" >/dev/null 2>&1; then
        pass "frozen modules unchanged (scal/bdef/ecu_write)"
    else
        fail "FROZEN MODULES MODIFIED — re-run firmware/test/verify_frozen.sh for details"
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
# Section 8 — Required test scenarios in test_sbf_orchestrator.c
# ---------------------------------------------------------------------------
section "8. Required test scenarios in test_sbf_orchestrator.c"

if [ -f "$TEST_FILE" ]; then
    declare -a REQUIRED=(
        "test_start_refuses_when_unpaid"
        "test_start_paid_applies_in_budget"
        "test_set_ethanol_triggers_reapply"
        "test_set_stage_triggers_reapply"
        "test_malformed_sbf_returns_idle"
        "test_swap_from_dtc"
        "test_apply_progress_events"
        "test_unload_drains_queue"
    )
    for pat in "${REQUIRED[@]}"; do
        if grep -Eq "$pat" "$TEST_FILE"; then
            pass "test file references scenario: $pat"
        else
            fail "test file missing scenario: $pat"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 9 — Cloud pytest regression (no cloud changes this prompt)
# ---------------------------------------------------------------------------
section "9. Cloud pytest regression"

if [ "${SKIP_PYTEST:-0}" = "1" ]; then
    echo "  SKIP  SKIP_PYTEST=1 set"
elif [ -d "$CLOUD_DIR/tests" ] && command -v python3 >/dev/null 2>&1; then
    if (cd "$CLOUD_DIR" && PYTHONPATH=. python3 -m pytest -x tests/ > /tmp/futuner_sbf_pytest.log 2>&1); then
        pass "cloud pytest passed"
    else
        fail "cloud pytest FAILED — see /tmp/futuner_sbf_pytest.log"
        tail -40 /tmp/futuner_sbf_pytest.log | sed 's/^/        /'
    fi
else
    echo "  SKIP  python3 / cloud tests dir unavailable"
fi

# ---------------------------------------------------------------------------
# Section 10 — Full firmware build (idf.py)
# ---------------------------------------------------------------------------
section "10. Full firmware build (idf.py)"

if [ "${SKIP_IDF_BUILD:-0}" = "1" ]; then
    echo "  SKIP  SKIP_IDF_BUILD=1 set"
elif [ -d "${IDF_PATH:-$HOME/esp/esp-idf}" ]; then
    if (cd "$FW_ROOT" && ./build.sh > /tmp/futuner_sbf_build.log 2>&1); then
        pass "idf.py build exited 0"
    else
        fail "idf.py build FAILED — see /tmp/futuner_sbf_build.log"
        tail -40 /tmp/futuner_sbf_build.log | sed 's/^/        /'
    fi
else
    echo "  SKIP  IDF_PATH not found"
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
    for f in "${FAILURES[@]}"; do echo "  - $f"; done
    echo
    echo "RESULT: FAIL"
    exit 1
fi

echo "RESULT: PASS"
echo "All checks green. You may declare Prompt 5 complete."
exit 0
