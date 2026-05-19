#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the WiFi mode-intent + STA-creds
# module + the new wifi_commands surface.
#
# Claude Code MUST run this and exit 0 before declaring the WiFi mode
# control prompt done.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/wifi_manager/eval.sh
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
WIFI_DIR="$SRC_ROOT/wifi"
CMD_DIR="$SRC_ROOT/commands"
CFG_HEADER="$SRC_ROOT/config/wifi_config.h"
TEST_FILE="$FW_ROOT/test/test_wifi_manager.c"
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
    "$WIFI_DIR/wifi_ap.h" \
    "$WIFI_DIR/wifi_ap.c" \
    "$CMD_DIR/wifi_commands.h" \
    "$CMD_DIR/wifi_commands.c" \
    "$CFG_HEADER" \
    "$TEST_FILE" \
    "$HOST_TEST_DIR/Makefile"
do
    if [ -f "$f" ]; then
        pass "exists: $(basename "$f")"
    else
        fail "missing required file: $f"
    fi
done

# ---------------------------------------------------------------------------
# Section 2 — Public API surface in wifi_ap.h
# ---------------------------------------------------------------------------
section "2. Public API surface"

if [ -f "$WIFI_DIR/wifi_ap.h" ]; then
    H="$WIFI_DIR/wifi_ap.h"
    for sym in \
        "wifi_client_set_creds" \
        "wifi_client_clear_creds" \
        "wifi_client_creds_stored" \
        "wifi_set_mode_intent" \
        "wifi_get_mode_intent" \
        "wifi_feature_uses_cloud_network" \
        "wifi_mode_intent_t"
    do
        if grep -q "$sym" "$H"; then
            pass "header declares: $sym"
        else
            fail "header missing symbol: $sym"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 3 — Magic-number relocation into wifi_config.h
# ---------------------------------------------------------------------------
section "3. Magic-number relocation"

if [ -f "$CFG_HEADER" ]; then
    for k in \
        "STA_MAX_RETRIES" \
        "WIFI_AP_CHANNEL" \
        "WIFI_AP_MAX_CONNECTIONS" \
        "WIFI_AP_PASSWORD_DEFAULT" \
        "WIFI_MODE_INTENT_NVS_KEY" \
        "WIFI_MODE_INTENT_AP_ONLY" \
        "WIFI_MODE_INTENT_APSTA"
    do
        if grep -q "$k" "$CFG_HEADER"; then
            pass "wifi_config.h defines: $k"
        else
            fail "wifi_config.h missing required define: $k"
        fi
    done

    # Every relocated value must carry the "DEFER LOCK UNTIL OWNER REVIEW"
    # annotation per the prompt's hard rule.
    if grep -qiE "DEFER LOCK UNTIL OWNER REVIEW" "$CFG_HEADER"; then
        pass "wifi_config.h flags defaults as needing owner sign-off"
    else
        fail "wifi_config.h missing 'DEFER LOCK UNTIL OWNER REVIEW' annotation"
    fi
fi

# Check that the formerly-inlined values are gone from wifi_ap.h / .c.
for orphan in "STA_MAX_RETRIES 5" "WIFI_AP_CHANNEL 1" "WIFI_AP_MAX_CONNECTIONS 4"; do
    if grep -F "#define $orphan" "$WIFI_DIR/wifi_ap.h" "$WIFI_DIR/wifi_ap.c" 2>/dev/null \
       | grep -v "wifi_config.h" >/dev/null; then
        fail "magic number still inline outside wifi_config.h: $orphan"
    else
        pass "no inline #define for: $orphan"
    fi
done

# ---------------------------------------------------------------------------
# Section 4 — Command surface wiring
# ---------------------------------------------------------------------------
section "4. Command surface wiring"

CMD_REGISTRY="$CMD_DIR/commands.c"
if [ -f "$CMD_REGISTRY" ]; then
    # Each new mutating cmd must be SECURED; wifi_status stays UNSECURED.
    declare -A want_tier=(
        ["wifi_sta_set"]="CMD_SECURITY_SECURED"
        ["wifi_mode"]="CMD_SECURITY_SECURED"
        ["wifi_clear"]="CMD_SECURITY_SECURED"
        ["wifi_status"]="CMD_SECURITY_UNSECURED"
    )
    for cmd in "${!want_tier[@]}"; do
        tier="${want_tier[$cmd]}"
        if grep -E "\"$cmd\"" "$CMD_REGISTRY" | grep -q "$tier"; then
            pass "$cmd registered with $tier"
        else
            fail "$cmd missing or wrong security tier (want $tier)"
        fi
    done
    # Legacy commands must remain registered per the owner-locked P-24 plan.
    for cmd in wifi_connect wifi_disconnect; do
        if grep -qE "\"$cmd\"" "$CMD_REGISTRY"; then
            pass "$cmd (legacy) still registered"
        else
            fail "$cmd (legacy) was removed — P-24 says leave intact"
        fi
    done
fi

SERIAL="$CMD_DIR/serial_console.c"
if [ -f "$SERIAL" ]; then
    for cmd in wifi_sta_set wifi_mode wifi_clear; do
        if grep -qE "\"$cmd\"" "$SERIAL"; then
            pass "$cmd dispatched in serial_console.c"
        else
            fail "$cmd missing from serial_console.c dispatcher"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 5 — Forbidden modifications
# ---------------------------------------------------------------------------
section "5. Forbidden modifications check"

FORBIDDEN=(
    "firmware/src/commands/command_handler.c"
    "firmware/src/commands/command_handler.h"
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/flash"
)

if command -v git >/dev/null 2>&1 && git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
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
            pass "untouched (allowlisted): $f"
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

if [ -f "$HOST_TEST_DIR/Makefile" ]; then
    if (cd "$HOST_TEST_DIR" && make -s clean && make -s); then
        pass "host test compiled"
    else
        fail "host test failed to compile"
    fi

    if [ -x "$HOST_TEST_DIR/host_test_runner" ]; then
        if (cd "$PROJECT_ROOT" && "$HOST_TEST_DIR/host_test_runner"); then
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
# Section 7 — Required test scenarios present
# ---------------------------------------------------------------------------
section "7. Required test scenarios in test_wifi_manager.c"

if [ -f "$TEST_FILE" ]; then
    declare -a REQUIRED=(
        "test_wifi_sta_set_stores_creds_in_nvs"
        "test_wifi_sta_set_rejects_short_password"
        "test_wifi_sta_set_rejects_empty_ssid"
        "test_wifi_mode_ap_clears_active_sta_connection"
        "test_wifi_mode_sta_fails_without_stored_creds"
        "test_wifi_mode_sta_invokes_existing_wifi_client_connect"
        "test_wifi_mode_sta_blocks_when_cloud_feature_active"
        "test_wifi_clear_removes_creds_and_forces_ap"
        "test_wifi_status_reflects_mode_and_creds_flag"
        "test_boot_skips_sta_when_intent_is_ap_only"
        "test_ws_server_starts_unconditionally_on_boot"
        "test_wifi_commands_require_auth_except_status"
    )
    for pat in "${REQUIRED[@]}"; do
        if grep -q "$pat" "$TEST_FILE"; then
            pass "test file defines scenario: $pat"
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
    exit 1
fi

echo "RESULT: PASS"
exit 0
