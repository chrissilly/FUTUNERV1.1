#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the MDG1 flash orchestrator.
#
# Claude Code MUST run this and exit 0 before declaring the orchestrator
# prompt done.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/mdg1_flash_orchestrator/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.
#
# Env knobs:
#   MM_CAPTURE_DIR=<path>   path to the directory holding MM captures
#                           (mm_FULL_Flash.log, mm_MAPS_upload.log) and
#                           the ECU bin. Default: /Users/rabbit/sniffer
#   SKIP_IDF_BUILD=1        skip the idf.py build step (host-only mode)
#   VERBOSE=1               print every check as it runs

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FW_ROOT="$PROJECT_ROOT/firmware"
SRC_ROOT="$FW_ROOT/src"
FLASH_DIR="$SRC_ROOT/flash"
CFG_DIR="$SRC_ROOT/config"
TEST_FILE="$SCRIPT_DIR/test_orchestrator.c"
HOST_TEST_DIR="$SCRIPT_DIR"
MM_DIR_DEFAULT="/Users/rabbit/sniffer"
MM_DIR="${MM_CAPTURE_DIR:-$MM_DIR_DEFAULT}"

cd "$PROJECT_ROOT"

PASS_COUNT=0
FAIL_COUNT=0
FAILURES=()

pass() { PASS_COUNT=$((PASS_COUNT+1)); [ "${VERBOSE:-0}" = "1" ] && echo "  PASS  $1"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); FAILURES+=("$1"); echo "  FAIL  $1"; }
section() {
    echo
    echo "=============================================================="
    echo "  $1"
    echo "=============================================================="
}

# ---------------------------------------------------------------------------
# 1. MM capture prerequisites
# ---------------------------------------------------------------------------
section "1. MM capture prerequisites"

if [ ! -f "$MM_DIR/mm_FULL_Flash.log" ]; then
    fail "mm_FULL_Flash.log missing at $MM_DIR — set MM_CAPTURE_DIR env var or symlink the captures (see firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md)"
    echo
    echo "RESULT: FAIL"
    exit 1
fi
pass "mm_FULL_Flash.log present at $MM_DIR"

if [ ! -f "$MM_DIR/WUAPCBF28NN902533_4K0907557G__0003.bin" ]; then
    fail "ECU bin missing at $MM_DIR — same fix as above"
    echo
    echo "RESULT: FAIL"
    exit 1
fi
pass "ECU bin present at $MM_DIR"

# ---------------------------------------------------------------------------
# 2. File structure
# ---------------------------------------------------------------------------
section "2. File structure"

REQUIRED_FILES=(
    "$FLASH_DIR/mdg1_flash_orchestrator.c"
    "$FLASH_DIR/mdg1_flash_orchestrator.h"
    "$FLASH_DIR/mdg1_uds_transport.h"
    "$FLASH_DIR/mdg1_transport_shadow.c"
    "$FLASH_DIR/mdg1_transport_shadow.h"
    "$FLASH_DIR/mdg1_transport_can.c"
    "$FLASH_DIR/mdg1_aes_mbedtls.c"
    "$FLASH_DIR/mdg1_variant_manifest.c"
    "$FLASH_DIR/mdg1_variant_manifest.h"
    "$CFG_DIR/mdg1_flash_orchestrator_config.h"
    "$TEST_FILE"
    "$SCRIPT_DIR/Makefile"
    "$PROJECT_ROOT/firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md"
    "$PROJECT_ROOT/firmware/test/can_capture/fixtures/expected_responses_4K0907557G_0003.json"
    "$PROJECT_ROOT/tools/flash_shadow_diff.py"
    "$PROJECT_ROOT/tools/extract_mm_expected_responses.py"
)
for f in "${REQUIRED_FILES[@]}"; do
    if [ -f "$f" ]; then
        pass "exists: ${f#$PROJECT_ROOT/}"
    else
        fail "missing: ${f#$PROJECT_ROOT/}"
    fi
done

# ---------------------------------------------------------------------------
# 3. Public API surface
# ---------------------------------------------------------------------------
section "3. Public API surface"

H="$FLASH_DIR/mdg1_flash_orchestrator.h"
if [ -f "$H" ]; then
    for sym in \
        "mdg1_flash_orchestrator_run" \
        "mdg1_flash_plan_t" \
        "mdg1_flash_progress_t" \
        "mdg1_flash_progress_cb_t" \
        "MDG1_FLASH_PHASE_INIT" \
        "MDG1_FLASH_PHASE_DONE"
    do
        if grep -q "$sym" "$H"; then
            pass "header declares: $sym"
        else
            fail "header missing: $sym"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 4. mdg1_flash_orchestrator_config.h
# ---------------------------------------------------------------------------
section "4. orchestrator config"

CFG="$CFG_DIR/mdg1_flash_orchestrator_config.h"
if [ -f "$CFG" ]; then
    if grep -qiE 'needs?\s+(approval|review)' "$CFG"; then
        pass "config flags defaults as needing approval"
    else
        fail "config should annotate proposed defaults"
    fi
    for key in \
        "MDG1_FLASH_CAN_ID_REQUEST" \
        "MDG1_FLASH_CAN_ID_RESPONSE" \
        "MDG1_UDS_SID_TRANSFER_DATA" \
        "MDG1_DATA_FORMAT_LZRB_AES" \
        "MDG1_ALFID_SIZE3_ADDR1" \
        "MDG1_MAX_FLASH_SECTIONS" \
        "MDG1_SHADOW_SECURITY_SEED_PLACEHOLDER"
    do
        if grep -qE "^\s*#define\s+$key\b" "$CFG"; then
            pass "config defines: $key"
        else
            fail "config missing required constant: $key"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 5. Forbidden modifications check (with overrides)
# ---------------------------------------------------------------------------
section "5. Forbidden modifications check"

FORBIDDEN=(
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/feature_manager/feature_manager.c"
    "firmware/src/dtc"
    "firmware/src/sbf"
    "firmware/src/vin_pairing"
    "firmware/src/license"
    "firmware/src/logger"
    "firmware/src/flex_fuel"
    "cloud"
    "ui"
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
            for u in "${unauthorized[@]}"; do echo "        unauthorized: $u"; done
        fi
    done
else
    echo "  SKIP  git not available"
fi

# ---------------------------------------------------------------------------
# 6. Frozen modules cross-check
# ---------------------------------------------------------------------------
section "6. Frozen modules check"

if [ -x "$PROJECT_ROOT/firmware/test/verify_frozen.sh" ]; then
    if "$PROJECT_ROOT/firmware/test/verify_frozen.sh" >/dev/null 2>&1; then
        pass "frozen modules unchanged (scal/bdef/ecu_write)"
    else
        fail "frozen modules modified — re-run firmware/test/verify_frozen.sh"
    fi
else
    fail "verify_frozen.sh missing or not executable"
fi

# ---------------------------------------------------------------------------
# 7. Host unit test compiles + runs
# ---------------------------------------------------------------------------
section "7. Host unit test (orchestrator)"

if [ -f "$HOST_TEST_DIR/Makefile" ]; then
    if (cd "$HOST_TEST_DIR" && make -s clean && make -s 2>&1); then
        pass "host test compiled"
    else
        fail "host test failed to compile"
    fi
    if [ -x "$HOST_TEST_DIR/host_test_runner" ]; then
        if "$HOST_TEST_DIR/host_test_runner" 2>&1 | tail -5 | grep -q "orchestrator host tests passed"; then
            pass "host_test_runner exited 0 (all scenarios pass)"
        else
            fail "host_test_runner reported failures"
        fi
    else
        fail "host_test_runner missing after build"
    fi
fi

# ---------------------------------------------------------------------------
# 8. Required test scenarios (15 literal names)
# ---------------------------------------------------------------------------
section "8. Required test scenarios"

declare -a REQUIRED_SCENARIOS=(
    "test_shadow_full_protocol_perfect_and_plaintext_equivalent"
    "test_shadow_cal_protocol_perfect_and_plaintext_equivalent"
    "test_orchestrator_5_sections_in_correct_order"
    "test_orchestrator_halts_on_unexpected_can_id"
    "test_orchestrator_aborts_on_key_fingerprint_mismatch"
    "test_orchestrator_feature_manager_off_blocks_start"
    "test_transport_interface_swappable_without_orchestrator_change"
    "test_variant_manifest_loader_validates_sha256"
    "test_variant_manifest_loader_rejects_missing_entry"
    "test_session_variant_mask_zeroes_seed_key_fingerprint"
    "test_diff_tool_exits_2_on_mismatch"
    "test_diff_tool_exits_0_on_match"
    "test_orchestrator_propagates_progress_callbacks"
    "test_hil_preflight_halt_before_erase_no_erase_emitted"
    "test_hil_defensive_secondary_engages_when_primary_bypassed"
)
for pat in "${REQUIRED_SCENARIOS[@]}"; do
    if grep -q "$pat" "$TEST_FILE"; then
        pass "test file references scenario: $pat"
    else
        fail "test file missing scenario: $pat"
    fi
done

# ---------------------------------------------------------------------------
# 9. flash_shadow_diff.py — protocol + plaintext-equivalent vs MM
# ---------------------------------------------------------------------------
section "9. flash_shadow_diff.py: shadow_full vs mm_FULL_Flash.log"

SHADOW_FULL="/tmp/shadow_full_4K0907557G_0003.log"
if [ ! -f "$SHADOW_FULL" ]; then
    # Generate it by running the host test
    "$HOST_TEST_DIR/host_test_runner" > /dev/null 2>&1 || true
fi

if [ ! -f "$SHADOW_FULL" ]; then
    fail "shadow log not produced — re-run host_test_runner first"
else
    if python3 "$PROJECT_ROOT/tools/flash_shadow_diff.py" \
        --shadow "$SHADOW_FULL" \
        --reference "$MM_DIR/mm_FULL_Flash.log" \
        --window flash-critical >/tmp/orch_diff.log 2>&1; then
        pass "shadow_full vs mm_FULL_Flash.log: PROTOCOL + PLAINTEXT MATCH"
    else
        fail "shadow_full vs mm_FULL_Flash.log: MISMATCH — see /tmp/orch_diff.log"
        tail -20 /tmp/orch_diff.log | sed 's/^/        /'
    fi
fi

# ---------------------------------------------------------------------------
# 10. CAL section cross-check against mm_MAPS_upload.log
# ---------------------------------------------------------------------------
section "10. CAL cross-check vs mm_MAPS_upload.log"

if [ ! -f "$MM_DIR/mm_MAPS_upload.log" ]; then
    echo "  SKIP  mm_MAPS_upload.log not present in $MM_DIR"
else
    if python3 "$PROJECT_ROOT/tools/flash_shadow_diff.py" \
        --shadow "$SHADOW_FULL" \
        --reference "$MM_DIR/mm_MAPS_upload.log" \
        --section CAL >/tmp/orch_diff_cal.log 2>&1; then
        pass "CAL section: PROTOCOL + PLAINTEXT MATCH vs MAPS-only capture"
    else
        fail "CAL section vs mm_MAPS_upload.log: MISMATCH — see /tmp/orch_diff_cal.log"
        tail -20 /tmp/orch_diff_cal.log | sed 's/^/        /'
    fi
fi

# ---------------------------------------------------------------------------
# 11. pyflakes on the diff tool
# ---------------------------------------------------------------------------
section "11. pyflakes on flash_shadow_diff.py"

if command -v pyflakes >/dev/null 2>&1; then
    if pyflakes "$PROJECT_ROOT/tools/flash_shadow_diff.py" 2>&1; then
        pass "pyflakes clean"
    else
        fail "pyflakes found issues"
    fi
else
    # Use python -m compile as a fallback static check.
    if python3 -m py_compile "$PROJECT_ROOT/tools/flash_shadow_diff.py" 2>&1; then
        pass "python3 -m py_compile clean (pyflakes not installed)"
    else
        fail "syntax errors in flash_shadow_diff.py"
    fi
fi

# ---------------------------------------------------------------------------
# 12. Full firmware build (idf.py) — optional, gated
# ---------------------------------------------------------------------------
section "12. Full firmware build"

if [ "${SKIP_IDF_BUILD:-0}" = "1" ]; then
    echo "  SKIP  SKIP_IDF_BUILD=1"
elif [ -d "${IDF_PATH:-$HOME/esp/esp-idf}" ]; then
    if (cd "$FW_ROOT" && ./build.sh > /tmp/orch_idf_build.log 2>&1); then
        pass "idf.py build exited 0 (Phase 2 OFF, default)"
    else
        fail "idf.py build FAILED — see /tmp/orch_idf_build.log"
        tail -30 /tmp/orch_idf_build.log | sed 's/^/        /'
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
exit 0
