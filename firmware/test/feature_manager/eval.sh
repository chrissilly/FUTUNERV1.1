#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the feature_manager module.
#
# Claude Code MUST run this and exit 0 before declaring Prompt 1 done.
# If any check fails, fix the issue and re-run. Do not declare success
# while any check is FAIL.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/feature_manager/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.
#
# Env knobs:
#   SKIP_IDF_BUILD=1   skip the full idf.py build step (useful in CI without IDF installed)
#   VERBOSE=1          print every check as it runs

set -u

# ---------------------------------------------------------------------------
# Locate the project root regardless of cwd.
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FW_ROOT="$PROJECT_ROOT/firmware"
SRC_ROOT="$FW_ROOT/src"
FM_DIR="$SRC_ROOT/feature_manager"
CFG_HEADER="$SRC_ROOT/config/feature_manager_config.h"
TEST_FILE="$FW_ROOT/test/test_feature_manager.c"
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
# Section 1 — File structure
# ---------------------------------------------------------------------------
section "1. File structure"

for f in \
    "$FM_DIR/feature_manager.h" \
    "$FM_DIR/feature_manager.c" \
    "$FM_DIR/CMakeLists.txt" \
    "$CFG_HEADER" \
    "$TEST_FILE"
do
    if [ -f "$f" ]; then
        pass "exists: $(basename "$f")"
    else
        fail "missing required file: $f"
    fi
done

# ---------------------------------------------------------------------------
# Section 2 — Public API surface in feature_manager.h
# ---------------------------------------------------------------------------
section "2. Public API surface"

if [ -f "$FM_DIR/feature_manager.h" ]; then
    H="$FM_DIR/feature_manager.h"
    for sym in \
        "FEATURE_NONE" \
        "FEATURE_COUNT" \
        "feature_id_t" \
        "feature_descriptor_t" \
        "feature_manager_init" \
        "feature_manager_register" \
        "feature_manager_request_start" \
        "feature_manager_request_stop" \
        "feature_manager_active" \
        "feature_manager_active_name"
    do
        if grep -q "$sym" "$H"; then
            pass "header declares: $sym"
        else
            fail "header missing symbol: $sym"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 3 — No-magic-numbers check on feature_manager.c
# ---------------------------------------------------------------------------
section "3. No magic numbers in feature_manager.c"

if [ -f "$FM_DIR/feature_manager.c" ]; then
    # Strip comments and strings, then scan for bare numeric literals other than 0/1.
    # We allow 0 and 1 (idiomatic), and we allow values that appear inside a
    # macro/#define line (those ARE the named constants). We also allow array
    # subscripts of [0] / [1].
    SUSPECT=$(
        sed -E 's://.*$::; s|/\*.*\*/||g' "$FM_DIR/feature_manager.c" \
        | grep -nE '\b[0-9]+\b' \
        | grep -vE '^[^:]+:[^:]*#define\b' \
        | grep -vE '\b[01]\b' \
        | grep -vE '\b(uint|int)(8|16|32|64)_t\b' \
        | grep -vE '0x0+' \
        || true
    )
    if [ -z "$SUSPECT" ]; then
        pass "no magic numbers detected in feature_manager.c"
    else
        fail "feature_manager.c contains numeric literals that look like magic numbers; move to feature_manager_config.h"
        echo "$SUSPECT" | sed 's/^/        /'
    fi
fi

# ---------------------------------------------------------------------------
# Section 4 — Config header has named constants
# ---------------------------------------------------------------------------
section "4. feature_manager_config.h uses named #defines"

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
fi

# ---------------------------------------------------------------------------
# Section 5 — Forbidden modifications
# ---------------------------------------------------------------------------
section "5. Forbidden modifications check"

# These files/directories must not be modified by Prompt 1.
FORBIDDEN=(
    "firmware/src/commands/command_handler.c"
    "firmware/src/commands/command_handler.h"
    "firmware/src/state_machine"
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/logger/wot_logger.c"
    "firmware/src/flash"
)

if command -v git >/dev/null 2>&1 && git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    # Load per-prompt overrides — paths listed in
    # firmware/test/_shared/eval_forbidden_overrides.txt are EXEMPT
    # from the FORBIDDEN check below. See that file's header for the
    # format + rationale.
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

# ---------------------------------------------------------------------------
# Section 6 — Host-side unit test compiles + runs
# ---------------------------------------------------------------------------
section "6. Host unit test"

# Claude Code is responsible for producing a host-runnable test driver at
# firmware/test/feature_manager/host_test_runner. The driver should
# exercise the public API per Prompt 1's acceptance criteria.
#
# This harness compiles the runner (if a Makefile is present) and runs it.

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
    fail "no host-test Makefile at $HOST_TEST_DIR/Makefile — Claude Code must produce one"
fi

# ---------------------------------------------------------------------------
# Section 7 — Required test cases present
# ---------------------------------------------------------------------------
section "7. Required test scenarios in test_feature_manager.c"

if [ -f "$TEST_FILE" ]; then
    # We grep for keywords that should appear in the test file's case names
    # or comments. Claude Code can use any naming convention that matches.
    declare -a REQUIRED=(
        "register"
        "start"
        "stop"
        "idempotent"
        "swap"
        "stop.*fail"
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
# Section 8 — Full firmware build still passes (optional, gated)
# ---------------------------------------------------------------------------
section "8. Full firmware build (idf.py)"

if [ "${SKIP_IDF_BUILD:-0}" = "1" ]; then
    echo "  SKIP  SKIP_IDF_BUILD=1 set; skipping idf.py build"
elif [ -d "${IDF_PATH:-$HOME/esp/esp-idf}" ]; then
    if (cd "$FW_ROOT" && ./build.sh > /tmp/futuner_build.log 2>&1); then
        pass "idf.py build exited 0"
    else
        fail "idf.py build FAILED — see /tmp/futuner_build.log"
        tail -40 /tmp/futuner_build.log | sed 's/^/        /'
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
echo "All checks green. You may declare Prompt 1 complete and hand back to Sean."
exit 0
