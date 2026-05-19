#!/usr/bin/env bash
# eval.sh — graded evaluation harness for mdg1_payload pack/unpack.
#
# Claude Code MUST run this and exit 0 before declaring this prompt
# done. If any check fails, fix the issue and re-run.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/mdg1_payload/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.
#
# Env knobs:
#   SKIP_IDF_BUILD=1   skip the full idf.py build step (CI without IDF)
#   VERBOSE=1          print every check as it runs

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FW_ROOT="$PROJECT_ROOT/firmware"
SRC_ROOT="$FW_ROOT/src"
FLASH_DIR="$SRC_ROOT/flash"
CFG_HEADER="$SRC_ROOT/config/mdg1_payload_config.h"
PUB_HEADER="$FLASH_DIR/mdg1_payload.h"
IMPL_FILE="$FLASH_DIR/mdg1_payload.c"
TEST_FILE="$FW_ROOT/test/test_mdg1_payload.c"
HOST_TEST_DIR="$SCRIPT_DIR"
FUTUNER_CFG="$SRC_ROOT/config/futuner_config.h"
KEYS_JSON="$PROJECT_ROOT/secrets/aes_keys_per_boxcode.json"

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
# 1. File structure
# ---------------------------------------------------------------------------
section "1. File structure"

REQUIRED_FILES=(
    "$PUB_HEADER"
    "$IMPL_FILE"
    "$CFG_HEADER"
    "$TEST_FILE"
    "$HOST_TEST_DIR/Makefile"
    "$HOST_TEST_DIR/tiny_aes.c"
    "$HOST_TEST_DIR/tiny_aes.h"
)
for f in "${REQUIRED_FILES[@]}"; do
    if [ -f "$f" ]; then
        pass "exists: ${f#$PROJECT_ROOT/}"
    else
        fail "missing required file: ${f#$PROJECT_ROOT/}"
    fi
done

# ---------------------------------------------------------------------------
# 2. Public API surface in mdg1_payload.h
# ---------------------------------------------------------------------------
section "2. Public API surface"

if [ -f "$PUB_HEADER" ]; then
    for sym in \
        "mdg1_aes_iface_t" \
        "mdg1_payload_set_aes_iface" \
        "mdg1_payload_get_aes_iface" \
        "mdg1_payload_pack" \
        "mdg1_payload_unpack"
    do
        if grep -q "$sym" "$PUB_HEADER"; then
            pass "header declares: $sym"
        else
            fail "header missing symbol: $sym"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 3. No magic numbers in mdg1_payload.c
# ---------------------------------------------------------------------------
section "3. No magic numbers in mdg1_payload.c"

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
        pass "no magic numbers in $(basename "$file")"
    else
        fail "$(basename "$file") contains numeric literals — move to mdg1_payload_config.h"
        echo "$suspect" | sed 's/^/        /'
    fi
}

scan_magic "$IMPL_FILE"

# ---------------------------------------------------------------------------
# 4. mdg1_payload_config.h declares named tunables + approval annotation
# ---------------------------------------------------------------------------
section "4. mdg1_payload_config.h"

if [ -f "$CFG_HEADER" ]; then
    DEFINE_COUNT=$(grep -cE '^\s*#define\s+\w+' "$CFG_HEADER" || true)
    if [ "$DEFINE_COUNT" -ge 1 ]; then
        pass "config header declares $DEFINE_COUNT named constant(s)"
    else
        fail "config header declares no #define constants"
    fi
    if grep -qiE 'needs?\s+(approval|review|sign-off)' "$CFG_HEADER"; then
        pass "config header annotates defaults as 'needs approval'"
    else
        fail "config header should annotate proposed defaults"
    fi
    for key in \
        "MDG1_PAYLOAD_AES_KEY_BYTES" \
        "MDG1_PAYLOAD_AES_BLOCK_BYTES" \
        "MDG1_PAYLOAD_PKCS7_MIN_PAD" \
        "MDG1_PAYLOAD_PKCS7_MAX_PAD" \
        "MDG1_BOSCH_FIXED_IV_INIT" \
        "MDG1_PAYLOAD_MAX_PLAINTEXT_BYTES" \
        "MDG1_PAYLOAD_MAX_CIPHERTEXT_BYTES"
    do
        if grep -qE "^\s*#define\s+$key\b" "$CFG_HEADER"; then
            pass "config defines $key"
        else
            fail "config header missing required constant: $key"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 5. Forbidden modifications check (Q7 of the kickoff prompt)
# ---------------------------------------------------------------------------
section "5. Forbidden modifications check"

# Per Q7 of the kickoff: this prompt must not touch frozen modules
# (scal/bdef/ecu_write — Section 6 enforces via SHA-256), prior features
# (feature_manager + every prior feature directory), or the variant
# manifest (per Q-C: aes_key block lives in aes_keys_per_boxcode.json
# instead). The flash/ tree intentionally NOT in this list — pre-existing
# untracked + dirty files in flash/ predate this prompt; the frozen-
# modules cross-check is the load-bearing guard for the files that
# actually matter for integrity.
FORBIDDEN=(
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/feature_manager"
    "firmware/src/dtc"
    "firmware/src/sbf"
    "firmware/src/vin_pairing"
    "firmware/src/license"
    "firmware/src/logger"
    "firmware/src/flex_fuel"
    "firmware/src/CMakeLists.txt"
    "secrets/mdg1_variant_manifest.json"
    "cloud"
    "firmware/test/verify_frozen.sh"
    "firmware/test/feature_manager"
    "firmware/test/wot_logger"
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
    echo "  SKIP  git not available — frozen-module cross-check is the load-bearing guard"
fi

# ---------------------------------------------------------------------------
# 6. Frozen modules cross-check
# ---------------------------------------------------------------------------
section "6. Frozen modules check"

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
# 7. Host-side unit test compiles + runs
# ---------------------------------------------------------------------------
section "7. Host unit test"

if [ -f "$HOST_TEST_DIR/Makefile" ]; then
    if (cd "$HOST_TEST_DIR" && make -s clean && make -s 2>&1); then
        pass "host test compiled"
    else
        fail "host test failed to compile"
    fi
    if [ -x "$HOST_TEST_DIR/host_test_runner" ]; then
        if "$HOST_TEST_DIR/host_test_runner"; then
            pass "host test runner exited 0"
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
# 8. Required test scenarios in test_mdg1_payload.c (9 literal names)
# ---------------------------------------------------------------------------
section "8. Required test scenarios"

if [ -f "$TEST_FILE" ]; then
    declare -a REQUIRED=(
        "test_pack_unpack_roundtrip_empty"
        "test_pack_unpack_roundtrip_1byte"
        "test_pack_unpack_roundtrip_64kb_random"
        "test_pack_unpack_roundtrip_4kb_zeros"
        "test_unpack_rs7_cal_against_oracle"
        "test_pkcs7_pad_strip_all_lengths_1_to_16"
        "test_aes_iface_injection"
        "test_lzrb_failure_returns_invalid_state"
        "test_oversize_input_returns_invalid_size"
    )
    for pat in "${REQUIRED[@]}"; do
        if grep -q "$pat" "$TEST_FILE"; then
            pass "test file references scenario: $pat"
        else
            fail "test file missing scenario: $pat"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 9. Companion: futuner_config.h declares FUTUNER_PHASE2_ENABLED
# ---------------------------------------------------------------------------
section "9. FUTUNER_PHASE2_ENABLED gate"

if [ -f "$FUTUNER_CFG" ]; then
    if grep -qE '^\s*#define\s+FUTUNER_PHASE2_ENABLED\b' "$FUTUNER_CFG"; then
        pass "futuner_config.h declares FUTUNER_PHASE2_ENABLED"
    else
        fail "futuner_config.h missing FUTUNER_PHASE2_ENABLED define"
    fi
fi

# ---------------------------------------------------------------------------
# 10. Companion: aes_keys_per_boxcode.json has aes_key block for RS7
# ---------------------------------------------------------------------------
section "10. aes_keys_per_boxcode.json schema extension"

if [ -f "$KEYS_JSON" ]; then
    if python3 -c "
import json, sys
d = json.load(open('$KEYS_JSON'))
entry = next((x for x in d if x.get('boxcode')=='4K0907557G__0003'), None)
if not entry:
    print('missing boxcode entry'); sys.exit(1)
ak = entry.get('aes_key')
if not isinstance(ak, dict):
    print('missing aes_key block'); sys.exit(1)
for k in ('source','bin_path','offset','length_bytes','sha256_first8_fingerprint'):
    if k not in ak:
        print(f'aes_key missing key: {k}'); sys.exit(1)
if ak.get('source') != 'bin_offset':
    print('aes_key.source != bin_offset'); sys.exit(1)
if ak.get('length_bytes') != 16:
    print('aes_key.length_bytes != 16'); sys.exit(1)
if ak.get('sha256_first8_fingerprint') != '7fa117fa':
    print('aes_key.sha256_first8_fingerprint != 7fa117fa'); sys.exit(1)
print('ok')
" 2>&1 | grep -q '^ok$'; then
        pass "RS7 aes_key block present and well-formed"
    else
        fail "RS7 aes_key block missing or malformed — re-check schema"
    fi
fi

# ---------------------------------------------------------------------------
# 11. Full firmware build (idf.py) — optional, gated
# ---------------------------------------------------------------------------
section "11. Full firmware build (idf.py)"

if [ "${SKIP_IDF_BUILD:-0}" = "1" ]; then
    echo "  SKIP  SKIP_IDF_BUILD=1 set; skipping idf.py build"
elif [ -d "${IDF_PATH:-$HOME/esp/esp-idf}" ]; then
    if (cd "$FW_ROOT" && ./build.sh > /tmp/futuner_mdg1_payload_build.log 2>&1); then
        pass "idf.py build exited 0 (default — Phase 2 OFF)"
    else
        fail "idf.py build FAILED — see /tmp/futuner_mdg1_payload_build.log"
        tail -40 /tmp/futuner_mdg1_payload_build.log | sed 's/^/        /'
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
