#!/usr/bin/env bash
# eval.sh — P-50 regression gate for the cloud_client factory.
#
# Catches the P-46 class of bugs (HTTPS client missing .crt_bundle_attach).
# Pure host-side static analysis — no IDF build needed.
#
# Usage:   bash firmware/test/cloud_client/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CLOUD_SRC="$PROJECT_ROOT/firmware/src/cloud"
FW_SRC="$PROJECT_ROOT/firmware/src"

cd "$PROJECT_ROOT"

PASS_COUNT=0
FAIL_COUNT=0
FAILURES=()

pass() { PASS_COUNT=$((PASS_COUNT+1)); echo "  PASS  $1"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); FAILURES+=("$1"); echo "  FAIL  $1"; }
section() { echo; echo "=============================================================="; echo "  $1"; echo "=============================================================="; }

# ---------------------------------------------------------------------------
section "1. cloud_client module present"
# ---------------------------------------------------------------------------
if [ -f "$CLOUD_SRC/cloud_client.h" ]; then
    pass "cloud_client.h exists"
else
    fail "cloud_client.h missing"
fi
if [ -f "$CLOUD_SRC/cloud_client.c" ]; then
    pass "cloud_client.c exists"
else
    fail "cloud_client.c missing"
fi

# ---------------------------------------------------------------------------
section "2. Factory entry point declared + defined"
# ---------------------------------------------------------------------------
if grep -qE 'esp_http_client_handle_t cloud_client_https_init' "$CLOUD_SRC/cloud_client.h" 2>/dev/null; then
    pass "cloud_client_https_init declared in header"
else
    fail "cloud_client_https_init declaration missing from cloud_client.h"
fi
if grep -qE 'esp_http_client_handle_t cloud_client_https_init' "$CLOUD_SRC/cloud_client.c" 2>/dev/null; then
    pass "cloud_client_https_init defined in cloud_client.c"
else
    fail "cloud_client_https_init definition missing from cloud_client.c"
fi

# ---------------------------------------------------------------------------
section "3. .crt_bundle_attach lives ONLY in cloud_client.c (P-49 audit)"
# ---------------------------------------------------------------------------
# This is the P-46 regression catcher. Any new HTTPS client elsewhere
# in firmware/src/ that re-inlines .crt_bundle_attach is a drift surface.
# Restrict to *.c so header doc-comments don't false-positive.
LEAKED=$(grep -rln --include='*.c' '\.crt_bundle_attach' "$FW_SRC" 2>/dev/null | grep -v 'cloud/cloud_client\.c')
if [ -z "$LEAKED" ]; then
    pass ".crt_bundle_attach is centralized in cloud_client.c only"
else
    fail ".crt_bundle_attach leaked outside cloud_client.c:"
    while IFS= read -r f; do
        echo "        $f"
    done <<< "$LEAKED"
fi

# ---------------------------------------------------------------------------
section "4. cloud_client.c attaches the bundle"
# ---------------------------------------------------------------------------
# The factory must include esp_crt_bundle and set .crt_bundle_attach.
# This is the literal P-46 regression: without the attach,
# esp-tls returns ESP_ERR_MBEDTLS_SSL_SETUP_FAILED.
if grep -qE '#[[:space:]]*include[[:space:]]+"esp_crt_bundle\.h"' "$CLOUD_SRC/cloud_client.c"; then
    pass "cloud_client.c includes esp_crt_bundle.h"
else
    fail "cloud_client.c missing #include \"esp_crt_bundle.h\" (P-46 regression risk)"
fi
if grep -qE '\.crt_bundle_attach[[:space:]]*=[[:space:]]*esp_crt_bundle_attach' "$CLOUD_SRC/cloud_client.c"; then
    pass "cloud_client.c assigns .crt_bundle_attach = esp_crt_bundle_attach"
else
    fail "cloud_client.c missing .crt_bundle_attach assignment (P-46 regression)"
fi

# ---------------------------------------------------------------------------
section "5. 3 known callers route through the factory"
# ---------------------------------------------------------------------------
# The original P-49 audit listed 3 modules + 4 call sites. Each module
# should now reference cloud/cloud_client.h via include AND call the
# factory at least once.
CALLERS=("vin_pairing/vin_pairing.c" "logger/wot_logger.c" "sbf/sbf_orchestrator.c")
for rel in "${CALLERS[@]}"; do
    f="$FW_SRC/$rel"
    if [ ! -f "$f" ]; then
        fail "$rel: file missing"
        continue
    fi
    if grep -qE '#[[:space:]]*include[[:space:]]+"cloud/cloud_client\.h"' "$f"; then
        pass "$rel includes cloud/cloud_client.h"
    else
        fail "$rel missing #include \"cloud/cloud_client.h\""
    fi
    if grep -qE 'cloud_client_https_init' "$f"; then
        pass "$rel calls cloud_client_https_init"
    else
        fail "$rel never calls cloud_client_https_init (raw esp_http_client_init drift risk)"
    fi
done

# ---------------------------------------------------------------------------
section "6. CMake registers cloud/cloud_client.c"
# ---------------------------------------------------------------------------
CMAKE="$FW_SRC/CMakeLists.txt"
if grep -qE 'cloud/cloud_client\.c' "$CMAKE"; then
    pass "firmware/src/CMakeLists.txt registers cloud/cloud_client.c"
else
    fail "firmware/src/CMakeLists.txt missing cloud/cloud_client.c (won't build)"
fi

# ---------------------------------------------------------------------------
echo
echo "=============================================================="
echo "  Summary"
echo "=============================================================="
echo "  Passed: $PASS_COUNT"
echo "  Failed: $FAIL_COUNT"
echo

if [ $FAIL_COUNT -eq 0 ]; then
    echo "RESULT: PASS"
    echo "All checks green. The P-46 regression surface is locked down — any future HTTPS client must route through cloud_client.c, and the factory must keep .crt_bundle_attach wired."
    exit 0
else
    echo "RESULT: FAIL"
    echo "Failures:"
    for f in "${FAILURES[@]}"; do echo "  - $f"; done
    exit 1
fi
