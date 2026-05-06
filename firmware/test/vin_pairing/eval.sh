#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the VIN pairing + license
# modules.
#
# Claude Code MUST run this and exit 0 before declaring this prompt
# done. If any check fails, fix the issue and re-run.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/vin_pairing/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.
#
# Env knobs:
#   SKIP_IDF_BUILD=1   skip the full idf.py build step (CI without IDF)
#   SKIP_PYTEST=1      skip the cloud-side pytest step
#   VERBOSE=1          print every check as it runs

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FW_ROOT="$PROJECT_ROOT/firmware"
SRC_ROOT="$FW_ROOT/src"
LIC_DIR="$SRC_ROOT/license"
VP_DIR="$SRC_ROOT/vin_pairing"
CMD_DIR="$SRC_ROOT/commands"
LIC_CFG="$SRC_ROOT/config/license_config.h"
VP_CFG="$SRC_ROOT/config/vin_pairing_config.h"
TEST_FILE="$FW_ROOT/test/test_vin_pairing.c"
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
    "$LIC_DIR/license.h"
    "$LIC_DIR/license.c"
    "$VP_DIR/vin_pairing.h"
    "$VP_DIR/vin_pairing.c"
    "$LIC_CFG"
    "$VP_CFG"
    "$CMD_DIR/vin_pair_commands.h"
    "$CMD_DIR/vin_pair_commands.c"
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
# Section 2 — Public API surface
# ---------------------------------------------------------------------------
section "2. Public API surface"

if [ -f "$LIC_DIR/license.h" ]; then
    H="$LIC_DIR/license.h"
    for sym in \
        "license_init" \
        "license_load_cache" \
        "license_save_cache" \
        "license_post_register" \
        "license_fetch" \
        "license_can_run_feature" \
        "license_normalize_vin" \
        "license_has_auth_token"
    do
        if grep -q "$sym" "$H"; then
            pass "license.h declares: $sym"
        else
            fail "license.h missing symbol: $sym"
        fi
    done
fi

if [ -f "$VP_DIR/vin_pairing.h" ]; then
    H="$VP_DIR/vin_pairing.h"
    for sym in \
        "vin_pairing_init" \
        "vin_pairing_register_with_feature_manager" \
        "vin_pairing_run_now" \
        "vin_pairing_is_running"
    do
        if grep -q "$sym" "$H"; then
            pass "vin_pairing.h declares: $sym"
        else
            fail "vin_pairing.h missing symbol: $sym"
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
        fail "$(basename "$file") contains numeric literals that look like magic numbers; move to *_config.h"
        echo "$suspect" | sed 's/^/        /'
    fi
}

scan_magic "$LIC_DIR/license.c"
scan_magic "$VP_DIR/vin_pairing.c"
scan_magic "$CMD_DIR/vin_pair_commands.c"

# ---------------------------------------------------------------------------
# Section 4 — Config headers have named #defines + approval/locked annotation
# ---------------------------------------------------------------------------
section "4. Config headers"

check_cfg() {
    local cfg="$1"
    [ -f "$cfg" ] || return 0
    local define_count
    define_count=$(grep -cE '^\s*#define\s+\w+' "$cfg" || true)
    if [ "$define_count" -ge 1 ]; then
        pass "$(basename "$cfg") declares $define_count named constant(s)"
    else
        fail "$(basename "$cfg") declares no #define constants"
    fi
    if grep -qiE 'needs?\s+(approval|review|sign-off)' "$cfg"; then
        pass "$(basename "$cfg") flags defaults as needing approval"
    elif grep -qE 'Locked\s+[0-9]{4}-[0-9]{2}-[0-9]{2}' "$cfg"; then
        pass "$(basename "$cfg") marked Locked with date stamp"
    else
        fail "$(basename "$cfg") should carry an approval-status marker — either 'needs approval/review/sign-off' (pre-lock) or 'Locked YYYY-MM-DD' (post-lock)"
    fi
}
check_cfg "$LIC_CFG"
check_cfg "$VP_CFG"

# Required keys per kickoff response.
for key in \
    "LICENSE_DEFAULT_HOST" \
    "LICENSE_REGISTER_PATH" \
    "LICENSE_LICENSE_PATH" \
    "LICENSE_NVS_PAID_KEY" \
    "LICENSE_NVS_VIN_KEY" \
    "LICENSE_NVS_REVOKED_KEY" \
    "LICENSE_NVS_AUTH_TOKEN_KEY"
do
    if grep -qE "^\s*#define\s+$key\b" "$LIC_CFG"; then
        pass "license_config.h defines $key"
    else
        fail "license_config.h missing required constant: $key"
    fi
done

# ---------------------------------------------------------------------------
# Section 5 — Forbidden modifications check
# ---------------------------------------------------------------------------
section "5. Forbidden modifications check"

# Frozen modules and predecessor-prompt deliverables MUST stay
# untouched. Connection_manager IS allowed to be modified (we add
# get_vin), so it's not on this list.
FORBIDDEN=(
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/flash"
    "firmware/src/feature_manager"
    # Granular: wot_uploader.{c,h} is in scope as of Prompt 5 (license-
    # gate one-liner per Q2). Keep the rest of firmware/src/logger/
    # off-limits.
    "firmware/src/logger/wot_logger.c"
    "firmware/src/logger/wot_logger.h"
    "firmware/src/logger/wot_recorder.c"
    "firmware/src/logger/wot_recorder.h"
    "firmware/src/logger/logger_manager.c"
    "firmware/src/logger/logger_manager.h"
    "firmware/src/logger/logger_config.c"
    "firmware/src/logger/logger_config.h"
    "firmware/src/logger/logger_profile.c"
    "firmware/src/logger/logger_profile.h"
    "firmware/src/logger/logger_variables.c"
    "firmware/src/logger/logger_variables.h"
    "firmware/src/dtc"
    "firmware/src/commands/command_handler.c"
    "firmware/src/commands/command_handler.h"
    "firmware/src/commands/wot_log_commands.c"
    "firmware/src/commands/wot_log_commands.h"
    "firmware/src/commands/dtc_commands.c"
    "firmware/src/commands/dtc_commands.h"
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
    "firmware/test/feature_manager"
    "firmware/test/test_feature_manager.c"
    "firmware/test/wot_logger"
    "firmware/test/test_wot_logger.c"
    "firmware/test/dtc"
    "firmware/test/test_dtc.c"
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
    echo "  SKIP  git not available — frozen-modules cross-check is the load-bearing guard"
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
# Section 8 — Required test scenarios in test_vin_pairing.c
# ---------------------------------------------------------------------------
section "8. Required test scenarios in test_vin_pairing.c"

if [ -f "$TEST_FILE" ]; then
    declare -a REQUIRED=(
        "(cold.*pair|cold_pair)"           # cold pairing
        "(warm.*pair|warm_pair)"           # warm pairing
        "(vin.*match|match)"               # VIN match
        "(mismatch)"                       # VIN mismatch
        "(revoked)"                        # license revoked
        "(wifi.*unavailable|wifi_un)"      # Wi-Fi unavailable
        "(409|skip.*license)"              # register 409 skips license
        "(401|cache.*clear)"               # 401 clears cache
        "(5xx|503|offline|grace)"          # 5xx offline grace
        "(normalize|ISO.3779|uppercase)"   # VIN normalization
        "(arbitrat)"                       # arbitration through feature_manager
        "(boxcode)"                        # boxcode source mock
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
# Section 9 — Cloud pytest
# ---------------------------------------------------------------------------
section "9. Cloud pytest"

if [ "${SKIP_PYTEST:-0}" = "1" ]; then
    echo "  SKIP  SKIP_PYTEST=1 set; skipping cloud pytest"
elif [ -d "$CLOUD_DIR/tests" ] && command -v python3 >/dev/null 2>&1; then
    if (cd "$CLOUD_DIR" && PYTHONPATH=. python3 -m pytest -x tests/ > /tmp/futuner_cloud_pytest.log 2>&1); then
        pass "cloud pytest passed"
    else
        fail "cloud pytest FAILED — see /tmp/futuner_cloud_pytest.log"
        tail -40 /tmp/futuner_cloud_pytest.log | sed 's/^/        /'
    fi
else
    echo "  SKIP  python3 or cloud/tests dir unavailable"
fi

# ---------------------------------------------------------------------------
# Section 10 — Full firmware build (idf.py)
# ---------------------------------------------------------------------------
section "10. Full firmware build (idf.py)"

if [ "${SKIP_IDF_BUILD:-0}" = "1" ]; then
    echo "  SKIP  SKIP_IDF_BUILD=1 set; skipping idf.py build"
elif [ -d "${IDF_PATH:-$HOME/esp/esp-idf}" ]; then
    if (cd "$FW_ROOT" && ./build.sh > /tmp/futuner_vin_build.log 2>&1); then
        pass "idf.py build exited 0"
    else
        fail "idf.py build FAILED — see /tmp/futuner_vin_build.log"
        tail -40 /tmp/futuner_vin_build.log | sed 's/^/        /'
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
echo "All checks green. You may declare the VIN pairing prompt complete and hand back to Sean."
exit 0
