#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the bench CAN toolkit.
#
# Tandem Claude Code session MUST run this and exit 0 before declaring
# the toolkit complete. If any check fails, fix the issue and re-run.
#
# This harness deliberately does NOT require a real Candlelight or any
# CAN hardware. It grades the toolkit against synthetic fixtures so it
# can be developed and self-verified entirely on the dev machine.
# Hardware-in-the-loop validation happens in a later, separate step
# once the toolkit's own eval is green.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/can_capture/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.

set -u

# ---------------------------------------------------------------------------
# Locate the project root regardless of cwd.
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
TOOLKIT_DIR="$SCRIPT_DIR"
BENCH_DIR="$TOOLKIT_DIR/bench"
FIXTURE_DIR="$TOOLKIT_DIR/fixtures"

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

REQUIRED_FILES=(
    "$BENCH_DIR/can_setup.sh"
    "$BENCH_DIR/can_teardown.sh"
    "$BENCH_DIR/capture_start.sh"
    "$BENCH_DIR/capture_stop.sh"
    "$BENCH_DIR/parse_uds.py"
    "$BENCH_DIR/cansend_safe.sh"
    "$TOOLKIT_DIR/README.md"
    "$TOOLKIT_DIR/requirements.txt"
)

for f in "${REQUIRED_FILES[@]}"; do
    if [ -f "$f" ]; then
        pass "exists: ${f#$PROJECT_ROOT/}"
    else
        fail "missing required file: ${f#$PROJECT_ROOT/}"
    fi
done

# Shell scripts must be executable.
for f in "$BENCH_DIR/can_setup.sh" "$BENCH_DIR/can_teardown.sh" \
         "$BENCH_DIR/capture_start.sh" "$BENCH_DIR/capture_stop.sh" \
         "$BENCH_DIR/cansend_safe.sh"; do
    if [ -x "$f" ]; then
        pass "executable: $(basename "$f")"
    elif [ -f "$f" ]; then
        fail "not executable: ${f#$PROJECT_ROOT/} (chmod +x)"
    fi
done

# ---------------------------------------------------------------------------
# Section 2 — Spec doc references
# ---------------------------------------------------------------------------
section "2. Documentation"

SPEC="$PROJECT_ROOT/docs/BENCH_CAN_TOOLKIT.md"
if [ -f "$SPEC" ]; then
    pass "spec doc exists: docs/BENCH_CAN_TOOLKIT.md"
else
    fail "spec doc missing: docs/BENCH_CAN_TOOLKIT.md"
fi

if [ -f "$TOOLKIT_DIR/README.md" ]; then
    if grep -q "BENCH_CAN_TOOLKIT.md" "$TOOLKIT_DIR/README.md"; then
        pass "README references the spec doc"
    else
        fail "README must reference docs/BENCH_CAN_TOOLKIT.md so users find the canonical spec"
    fi
fi

# ---------------------------------------------------------------------------
# Section 3 — Safety guard rejects tx without flags
# ---------------------------------------------------------------------------
section "3. Safety: cansend_safe.sh refuses tx without --allow-tx --target=bench"

if [ -x "$BENCH_DIR/cansend_safe.sh" ]; then
    # No flags at all → must refuse, non-zero exit.
    if "$BENCH_DIR/cansend_safe.sh" can0 7E0#0102030405060708 >/dev/null 2>&1; then
        fail "cansend_safe.sh allowed tx with no flags (must refuse)"
    else
        pass "cansend_safe.sh refused tx with no flags"
    fi

    # Only --allow-tx, missing --target → must refuse.
    if "$BENCH_DIR/cansend_safe.sh" --allow-tx can0 7E0#01 >/dev/null 2>&1; then
        fail "cansend_safe.sh allowed tx without --target=bench (must refuse)"
    else
        pass "cansend_safe.sh refused tx without --target=bench"
    fi

    # Wrong --target → must refuse.
    if "$BENCH_DIR/cansend_safe.sh" --allow-tx --target=car can0 7E0#01 >/dev/null 2>&1; then
        fail "cansend_safe.sh allowed tx with --target=car (must refuse — only bench allowed)"
    else
        pass "cansend_safe.sh refused tx with --target=car"
    fi
fi

# ---------------------------------------------------------------------------
# Section 4 — Synthetic fixtures present
# ---------------------------------------------------------------------------
section "4. Synthetic fixtures"

if [ -d "$FIXTURE_DIR" ]; then
    pass "fixtures/ directory exists"

    # At least one fixture trio (.candump, .expected.jsonl, .notes.md) required.
    FIXTURE_TRIOS=0
    for cd_file in "$FIXTURE_DIR"/*.candump; do
        [ -f "$cd_file" ] || continue
        base="${cd_file%.candump}"
        if [ -f "${base}.expected.jsonl" ] && [ -f "${base}.notes.md" ]; then
            FIXTURE_TRIOS=$((FIXTURE_TRIOS+1))
            pass "fixture trio: $(basename "$base")"
        else
            fail "fixture incomplete (need .candump, .expected.jsonl, .notes.md): $(basename "$base")"
        fi
    done

    if [ "$FIXTURE_TRIOS" -eq 0 ]; then
        fail "no complete fixture trios in fixtures/ — at least one is required"
    fi

    # The starter fixture from the spec must exist.
    if [ ! -f "$FIXTURE_DIR/read_vin.candump" ]; then
        fail "starter fixture missing: fixtures/read_vin.candump (per BENCH_CAN_TOOLKIT.md)"
    fi
else
    fail "fixtures/ directory missing"
fi

# ---------------------------------------------------------------------------
# Section 5 — Parser produces expected output for each fixture
# ---------------------------------------------------------------------------
section "5. Parser correctness"

if [ -f "$BENCH_DIR/parse_uds.py" ] && [ -d "$FIXTURE_DIR" ]; then
    if ! command -v python3 >/dev/null 2>&1; then
        fail "python3 not available; cannot run parser"
    else
        for cd_file in "$FIXTURE_DIR"/*.candump; do
            [ -f "$cd_file" ] || continue
            base="${cd_file%.candump}"
            expected="${base}.expected.jsonl"
            [ -f "$expected" ] || continue

            # Run parser, normalize timestamps to "TS" so fixtures can be
            # stable across runs without locking absolute timestamps.
            actual="$(python3 "$BENCH_DIR/parse_uds.py" "$cd_file" 2>/dev/null \
                | python3 -c '
import json, sys
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    obj = json.loads(line)
    obj["ts"] = "TS"
    print(json.dumps(obj, sort_keys=True))
')"
            expected_norm="$(python3 -c '
import json, sys
with open(sys.argv[1]) as f:
    for line in f:
        line = line.strip()
        if not line: continue
        obj = json.loads(line)
        obj["ts"] = "TS"
        print(json.dumps(obj, sort_keys=True))
' "$expected")"

            if [ "$actual" = "$expected_norm" ]; then
                pass "parser output matches: $(basename "$base")"
            else
                fail "parser output mismatch for $(basename "$base")"
                echo "        expected:"
                echo "$expected_norm" | sed 's/^/          /'
                echo "        actual:"
                echo "$actual" | sed 's/^/          /'
            fi
        done
    fi
fi

# ---------------------------------------------------------------------------
# Section 6 — Forbidden modifications (must stay inside the sandbox)
# ---------------------------------------------------------------------------
section "6. Sandbox containment"

# The tandem session is allowed to write inside firmware/test/can_capture/
# and docs/BENCH_CAN_TOOLKIT.md ONLY. Anywhere else is out of bounds.
FORBIDDEN_GLOBS=(
    "firmware/src"
    "firmware/components"
    "firmware/main"
    "firmware/include"
    "firmware/lib"
    "firmware/CMakeLists.txt"
    "firmware/test/feature_manager"
    "firmware/test/test_feature_manager.c"
    "cloud"
    "ui"
    "secrets"
    "sbf"
    "tools"
)

if command -v git >/dev/null 2>&1 && git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    for path in "${FORBIDDEN_GLOBS[@]}"; do
        if git -C "$PROJECT_ROOT" status --porcelain "$path" 2>/dev/null | grep -q '.'; then
            fail "out-of-sandbox modification: $path"
        else
            pass "untouched: $path"
        fi
    done
else
    echo "  SKIP  git not available — cannot verify sandbox containment"
fi

# Frozen modules check piggybacks on the existing harness.
if [ -x "$PROJECT_ROOT/firmware/test/verify_frozen.sh" ]; then
    if "$PROJECT_ROOT/firmware/test/verify_frozen.sh" >/dev/null 2>&1; then
        pass "frozen modules unchanged (scal/bdef/ecu_write)"
    else
        fail "frozen modules modified — see firmware/test/verify_frozen.sh output"
    fi
fi

# ---------------------------------------------------------------------------
# Section 7 — Python deps importable
# ---------------------------------------------------------------------------
section "7. Python dependencies"

if [ -f "$TOOLKIT_DIR/requirements.txt" ]; then
    if command -v python3 >/dev/null 2>&1; then
        # Try to import each top-level package listed.
        # Only check non-empty, non-comment lines.
        while IFS= read -r dep; do
            [ -z "$dep" ] && continue
            [[ "$dep" =~ ^# ]] && continue
            pkg="$(echo "$dep" | sed -E 's/[<>=!~].*$//' | sed 's/-/_/g' | tr -d '[:space:]')"
            [ -z "$pkg" ] && continue
            if python3 -c "import $pkg" 2>/dev/null; then
                pass "importable: $pkg"
            else
                fail "Python dependency not importable: $pkg (pip install -r requirements.txt)"
            fi
        done < "$TOOLKIT_DIR/requirements.txt"
    fi
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
echo "All checks green. Toolkit is ready for hardware-in-the-loop validation."
exit 0
