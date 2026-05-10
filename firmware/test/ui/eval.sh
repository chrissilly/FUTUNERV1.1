#!/usr/bin/env bash
# eval.sh — graded evaluation harness for the UI catchup sweep
# (Prompt 9). Three-layer:
#   A. Static checks (file structure, panel IDs, CSS tokens, command +
#      event coverage), greps + node --check + python3 -m py_compile.
#   B. JS / Python syntax validation.
#   C. WS + admin-HTTP round-trip against a fixture-driven mock dongle
#      that replays scripted event sequences.
#
# Claude Code MUST run this and exit 0 before declaring this prompt
# done. If any check fails, fix and re-run.
#
# Usage:   cd ~/esp/obd/FUTV1.1 && firmware/test/ui/eval.sh
# Returns: 0 on all-pass, non-zero on any failure.
#
# Env knobs:
#   SKIP_IDF_BUILD=1   skip the full idf.py build step
#   SKIP_PYTEST=1      skip the cloud-side pytest regression
#   VERBOSE=1          print every check as it runs
#   UI_EVAL_KEEP_LOGS=1 keep /tmp/futuner_ui_*.log on success

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FW_ROOT="$PROJECT_ROOT/firmware"
SRC_ROOT="$FW_ROOT/src"
CMD_DIR="$SRC_ROOT/commands"
UI_DIR="$PROJECT_ROOT/ui"
UI_TEST_DIR="$UI_DIR/test"
TOOLS_DIR="$PROJECT_ROOT/tools"
CLOUD_DIR="$PROJECT_ROOT/cloud"

UI_HTML="$UI_DIR/control_panel.html"
UI_CSS="$UI_DIR/control_panel.css"
UI_JS="$UI_DIR/control_panel.js"
FW_HTML="$FW_ROOT/futuner_control_panel.html"
BUNDLE_SCRIPT="$TOOLS_DIR/bundle_ui.py"
COMMANDS_C="$CMD_DIR/commands.c"

# Mock dongle ports — Proposed defaults; needs Sean's approval before lock.
MOCK_DONGLE_DEFAULT_PORT=47821
MOCK_DONGLE_ADMIN_PORT=47822
# Mock dongle warmup wait before launching the test client (sec).
MOCK_DONGLE_WARMUP_SEC=0.7
# Mock dongle hard-kill timeout if SIGTERM doesn't take (sec).
MOCK_DONGLE_KILL_TIMEOUT_SEC=2

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

# Cleanup hook so a failed mock spawn doesn't orphan a Python process.
MOCK_PID=""
cleanup() {
    if [ -n "$MOCK_PID" ] && kill -0 "$MOCK_PID" 2>/dev/null; then
        kill "$MOCK_PID" 2>/dev/null || true
        # Give it a moment to exit cleanly.
        for _i in 1 2 3 4 5; do
            kill -0 "$MOCK_PID" 2>/dev/null || break
            sleep 0.2
        done
        kill -9 "$MOCK_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Section 1 — File structure
# ---------------------------------------------------------------------------
section "1. File structure"

REQUIRED_FILES=(
    "$UI_HTML"
    "$UI_CSS"
    "$UI_JS"
    "$FW_HTML"
    "$BUNDLE_SCRIPT"
    "$UI_TEST_DIR/mock_dongle.py"
    "$UI_TEST_DIR/mock_dongle_responses.json"
    "$UI_TEST_DIR/test_round_trip.py"
    "$UI_TEST_DIR/README.md"
)
for f in "${REQUIRED_FILES[@]}"; do
    if [ -f "$f" ]; then pass "exists: ${f#$PROJECT_ROOT/}"
    else                  fail "missing required file: ${f#$PROJECT_ROOT/}"
    fi
done

# ---------------------------------------------------------------------------
# Section 2 — Layer A: HTML structure (canonical AND bundled).
# ---------------------------------------------------------------------------
section "2. HTML structure check"

REQUIRED_PANEL_IDS=(
    panel-dashboard panel-sniffer panel-diag panel-tuning panel-livetune
    panel-logconfig panel-files panel-wot panel-vinpair panel-system
)
HEADER_IDS=( licenseLock activeFeatureLabel )
MODAL_ID=swapConfirmModal

if [ -f "$UI_HTML" ]; then
    for id in "${REQUIRED_PANEL_IDS[@]}" "${HEADER_IDS[@]}" "$MODAL_ID"; do
        if grep -q "id=\"$id\"" "$UI_HTML"; then pass "canonical HTML has #$id"
        else                                      fail "canonical HTML missing #$id"
        fi
    done
fi
if [ -f "$FW_HTML" ]; then
    for id in "${REQUIRED_PANEL_IDS[@]}" "${HEADER_IDS[@]}" "$MODAL_ID"; do
        if grep -q "id=\"$id\"" "$FW_HTML"; then pass "bundled HTML has #$id"
        else                                       fail "bundled HTML missing #$id"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 3 — Bundle determinism (run twice, byte-compare).
# ---------------------------------------------------------------------------
section "3. Bundle determinism"

if [ -f "$BUNDLE_SCRIPT" ]; then
    BUNDLE_TMP_A=$(mktemp -t futuner_bundle_a.XXXXXX.html)
    BUNDLE_TMP_B=$(mktemp -t futuner_bundle_b.XXXXXX.html)
    if python3 "$BUNDLE_SCRIPT" --in "$UI_HTML" "$UI_CSS" "$UI_JS" --out "$BUNDLE_TMP_A" >/dev/null 2>&1 \
    && python3 "$BUNDLE_SCRIPT" --in "$UI_HTML" "$UI_CSS" "$UI_JS" --out "$BUNDLE_TMP_B" >/dev/null 2>&1; then
        if cmp -s "$BUNDLE_TMP_A" "$BUNDLE_TMP_B"; then
            pass "bundle is deterministic (byte-identical across 2 runs)"
        else
            fail "bundle output differs between runs — check tools/bundle_ui.py for non-determinism"
        fi
        if cmp -s "$BUNDLE_TMP_A" "$FW_HTML"; then
            pass "committed firmware/futuner_control_panel.html matches the bundle output"
        else
            fail "committed bundle does NOT match a fresh build — re-run tools/bundle_ui.py"
        fi
    else
        fail "tools/bundle_ui.py exited non-zero on a fresh run"
    fi
    rm -f "$BUNDLE_TMP_A" "$BUNDLE_TMP_B"
else
    fail "tools/bundle_ui.py missing — cannot verify determinism"
fi

# ---------------------------------------------------------------------------
# Section 4 — Layer A: command + event registry coverage (eval-side
# greps; the round-trip client also asserts these in scenario-1 and -2).
# ---------------------------------------------------------------------------
section "4. Command + event registry coverage"

# Commands not bound by Prompt 9's UI surface — exempted from the
# wsSend coverage check. Mirrors EVAL_COMMAND_EXEMPTIONS in
# ui/test/test_round_trip.py and _eval_command_exemptions in
# mock_dongle_responses.json.
declare -a EVAL_COMMAND_EXEMPTIONS=(
    pair_ecu remove_pairing configure_logger get_single_variable
    flex_load_scal flex_unload_scal flex_status flex_enable flex_disable flex_set_override
    logger_start logger_stop fs_info fs_mkdir list_available_vars
    delete_logger_profile can_sniff_status wifi_status
)
exempt() {
    local cmd="$1"
    for e in "${EVAL_COMMAND_EXEMPTIONS[@]}"; do
        [ "$e" = "$cmd" ] && return 0
    done
    return 1
}

if [ -f "$COMMANDS_C" ] && [ -f "$UI_JS" ]; then
    # Extract every quoted command name from the registry: lines like
    # `{"pair_ecu", "Pair with current ECU", ...},`. Pick the first
    # quoted token per row.
    REGISTRY_CMDS=$(awk '/COMMAND_REGISTRY\[\] = \{/{flag=1;next} /^};/{flag=0} flag && /\{\s*"/{
        match($0, /"[a-z_][a-z0-9_]*"/);
        if (RSTART > 0) {
            tok = substr($0, RSTART+1, RLENGTH-2);
            print tok;
        }
    }' "$COMMANDS_C")

    for cmd in $REGISTRY_CMDS; do
        if exempt "$cmd"; then
            [ "${VERBOSE:-0}" = "1" ] && echo "  SKIP  exempt: $cmd"
            continue
        fi
        # Grep the JS for either single- or double-quoted command:NAME.
        if grep -qE "command\s*:\s*['\"]${cmd}['\"]" "$UI_JS"; then
            pass "JS binds command: $cmd"
        else
            fail "JS missing wsSend binding for command: $cmd"
        fi
    done
else
    fail "commands.c or control_panel.js missing — cannot enumerate registry"
fi

# Events: pull every emit_event(...) literal from firmware/src.
if [ -f "$UI_JS" ]; then
    EMITTED_EVENTS=$(grep -rhoE '\\"event\\":\\"[a-z_][a-z0-9_]*\\"' "$SRC_ROOT" 2>/dev/null \
        | sed -E 's/.*\\"event\\":\\"([a-z_][a-z0-9_]*)\\".*/\1/' \
        | sort -u)
    for ev in $EMITTED_EVENTS; do
        # can_frame stays on the hot direct path (sniffer rate).
        if [ "$ev" = "can_frame" ]; then
            [ "${VERBOSE:-0}" = "1" ] && echo "  SKIP  exempt event: $ev (hot path)"
            continue
        fi
        if grep -qE "wsEvents\.on\s*\(\s*['\"]${ev}['\"]" "$UI_JS"; then
            pass "JS subscribes wsEvents.on('$ev')"
        else
            fail "JS missing wsEvents.on('$ev') — firmware emits this event"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 5 — Layer A: CSS tokens (no inline hex literals in new rules).
# ---------------------------------------------------------------------------
section "5. CSS tokens — no new inline hex literals"

if [ -f "$UI_CSS" ]; then
    # Strip :root { ... } block(s) and /* ... */ comments, then look
    # for hex color literals. The round-trip test does the heavy
    # lifting (with a known-legacy-literals allowlist); this fast
    # check only verifies the named forward-compat marker is present.
    if grep -q -- "--theme-name" "$UI_CSS"; then
        pass "control_panel.css declares --theme-name forward-compat token"
    else
        fail "control_panel.css missing --theme-name marker (Q6: forward-compat)"
    fi
    if grep -q -- "--modal-overlay" "$UI_CSS"; then
        pass "control_panel.css declares --modal-overlay token"
    else
        fail "control_panel.css missing --modal-overlay token"
    fi
    if grep -q -- "--touch-target-min" "$UI_CSS"; then
        pass "control_panel.css declares --touch-target-min token (≥44px iOS HIG)"
    else
        fail "control_panel.css missing --touch-target-min token"
    fi
    # The orange/black theme rule (Q6).
    if grep -qE "^\s*--accent:\s*#ff6600" "$UI_CSS"; then
        pass "--accent value preserved (orange/black theme)"
    else
        fail "--accent value changed — Q6 forbids theme migration"
    fi
fi

# ---------------------------------------------------------------------------
# Section 6 — Layer B: syntax checks.
# ---------------------------------------------------------------------------
section "6. JS / Python syntax"

if command -v node >/dev/null 2>&1; then
    if node --check "$UI_JS" >/dev/null 2>&1; then
        pass "node --check ui/control_panel.js"
    else
        fail "node --check ui/control_panel.js — syntax error"
        node --check "$UI_JS" 2>&1 | head -10 | sed 's/^/        /'
    fi
else
    fail "node not on PATH — cannot syntax-check the bundled JS"
fi

if command -v python3 >/dev/null 2>&1; then
    if python3 -m py_compile "$UI_TEST_DIR/mock_dongle.py" >/dev/null 2>&1; then
        pass "python3 -m py_compile ui/test/mock_dongle.py"
    else
        fail "ui/test/mock_dongle.py syntax error"
    fi
    if python3 -m py_compile "$UI_TEST_DIR/test_round_trip.py" >/dev/null 2>&1; then
        pass "python3 -m py_compile ui/test/test_round_trip.py"
    else
        fail "ui/test/test_round_trip.py syntax error"
    fi
    if python3 -m py_compile "$BUNDLE_SCRIPT" >/dev/null 2>&1; then
        pass "python3 -m py_compile tools/bundle_ui.py"
    else
        fail "tools/bundle_ui.py syntax error"
    fi
fi

# ---------------------------------------------------------------------------
# Section 7 — Layer A: required test scenarios literally appear in
# test_round_trip.py (the eval scenario-grep — Sean's literal names).
# ---------------------------------------------------------------------------
section "7. Required test scenarios in test_round_trip.py"

REQUIRED_SCENARIOS=(
    test_command_registry_coverage
    test_event_handler_coverage
    test_apply_progress_sequence
    test_apply_failed_path
    test_license_unpaid_refusal
    test_feature_swap_modal_appears
    test_dtc_read_renders_table
    test_html_structure_intact
    test_css_tokens_used
)
TEST_FILE="$UI_TEST_DIR/test_round_trip.py"
if [ -f "$TEST_FILE" ]; then
    for s in "${REQUIRED_SCENARIOS[@]}"; do
        if grep -qE "\b$s\b" "$TEST_FILE"; then
            pass "scenario present: $s"
        else
            fail "scenario MISSING from test_round_trip.py: $s"
        fi
    done
fi

# Required event sequences in the fixture.
REQUIRED_SEQS=(apply_progress_3_then_complete apply_failed_unload license_revoked_then_paid)
FIX_FILE="$UI_TEST_DIR/mock_dongle_responses.json"
if [ -f "$FIX_FILE" ]; then
    for s in "${REQUIRED_SEQS[@]}"; do
        if grep -q "\"$s\"" "$FIX_FILE"; then
            pass "fixture defines sequence: $s"
        else
            fail "fixture missing required sequence: $s"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Section 8 — Layer C: WS + admin-HTTP round-trip.
# ---------------------------------------------------------------------------
section "8. Mock-dongle round trip"

# Pre-flight: websockets installed?
if ! python3 -c "import websockets" >/dev/null 2>&1; then
    fail "Python 'websockets' package not installed — pip install --user websockets"
else
    MOCK_LOG=/tmp/futuner_ui_mock.log
    CLIENT_LOG=/tmp/futuner_ui_client.log
    rm -f "$MOCK_LOG" "$CLIENT_LOG"

    python3 "$UI_TEST_DIR/mock_dongle.py" \
        --port "$MOCK_DONGLE_DEFAULT_PORT" \
        --admin-port "$MOCK_DONGLE_ADMIN_PORT" \
        --fixture "$FIX_FILE" \
        --log-level WARNING \
        > "$MOCK_LOG" 2>&1 &
    MOCK_PID=$!

    sleep "$MOCK_DONGLE_WARMUP_SEC"

    if ! kill -0 "$MOCK_PID" 2>/dev/null; then
        fail "mock_dongle.py failed to start — see $MOCK_LOG"
        tail -30 "$MOCK_LOG" 2>/dev/null | sed 's/^/        /'
    else
        if python3 "$UI_TEST_DIR/test_round_trip.py" \
            --ws "ws://127.0.0.1:$MOCK_DONGLE_DEFAULT_PORT/" \
            --admin "http://127.0.0.1:$MOCK_DONGLE_ADMIN_PORT/" \
            --project-root "$PROJECT_ROOT" \
            > "$CLIENT_LOG" 2>&1; then
            if grep -q "^RESULT: PASS" "$CLIENT_LOG"; then
                pass "test_round_trip.py reported RESULT: PASS"
                if [ "${VERBOSE:-0}" = "1" ]; then
                    sed 's/^/        /' "$CLIENT_LOG"
                fi
            else
                fail "test_round_trip.py exited 0 but didn't print RESULT: PASS"
                tail -30 "$CLIENT_LOG" | sed 's/^/        /'
            fi
        else
            fail "test_round_trip.py reported failures — see $CLIENT_LOG"
            tail -50 "$CLIENT_LOG" | sed 's/^/        /'
        fi
    fi
    cleanup
    MOCK_PID=""

    if [ "${UI_EVAL_KEEP_LOGS:-0}" != "1" ] && [ "$FAIL_COUNT" -eq 0 ]; then
        rm -f "$MOCK_LOG" "$CLIENT_LOG"
    fi
fi

# ---------------------------------------------------------------------------
# Section 9 — Forbidden modifications check (UI-only — firmware/src/,
# firmware/test/, frozen modules, cloud must all be untouched).
# ---------------------------------------------------------------------------
section "9. Forbidden modifications check"

FORBIDDEN=(
    "firmware/src/scal"
    "firmware/src/bdef"
    "firmware/src/ecu_write"
    "firmware/src/flash"
    "firmware/src/feature_manager"
    "firmware/src/license"
    "firmware/src/vin_pairing"
    "firmware/src/dtc"
    "firmware/src/sbf"
    "firmware/src/logger"
    "firmware/src/commands"
    "firmware/src/CMakeLists.txt"
    "firmware/test/feature_manager"
    "firmware/test/wot_logger"
    "firmware/test/dtc"
    "firmware/test/vin_pairing"
    "firmware/test/sbf"
    "firmware/test/test_feature_manager.c"
    "firmware/test/test_wot_logger.c"
    "firmware/test/test_dtc.c"
    "firmware/test/test_vin_pairing.c"
    "firmware/test/test_sbf_orchestrator.c"
    "firmware/test/verify_frozen.sh"
    "cloud/src"
    "cloud/tests"
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
# Section 10 — Frozen modules cross-check (CRITICAL).
# ---------------------------------------------------------------------------
section "10. Frozen modules check (verify_frozen.sh)"

if [ -x "$FW_ROOT/test/verify_frozen.sh" ]; then
    if "$FW_ROOT/test/verify_frozen.sh" >/dev/null 2>&1; then
        pass "frozen modules unchanged (scal/bdef/ecu_write)"
    else
        fail "FROZEN MODULES MODIFIED — re-run firmware/test/verify_frozen.sh for details"
    fi
else
    fail "verify_frozen.sh missing or not executable"
fi

# ---------------------------------------------------------------------------
# Section 11 — Cross-prompt regression: every prior eval still PASSes.
# ---------------------------------------------------------------------------
section "11. Cross-prompt regression — prior eval gates"

PRIOR_GATES=(feature_manager wot_logger dtc vin_pairing sbf)
for g in "${PRIOR_GATES[@]}"; do
    GATE="$FW_ROOT/test/$g/eval.sh"
    if [ -x "$GATE" ]; then
        # We only care about RESULT: PASS — not the verbose log.
        # Each prior gate runs verify_frozen + its host test.
        if SKIP_IDF_BUILD="${SKIP_IDF_BUILD:-1}" SKIP_PYTEST="${SKIP_PYTEST:-1}" \
           "$GATE" > "/tmp/futuner_ui_regress_$g.log" 2>&1; then
            pass "prior gate clean: $g"
            [ "${UI_EVAL_KEEP_LOGS:-0}" != "1" ] && rm -f "/tmp/futuner_ui_regress_$g.log"
        else
            fail "prior gate REGRESSED: $g — see /tmp/futuner_ui_regress_$g.log"
            tail -30 "/tmp/futuner_ui_regress_$g.log" | sed 's/^/        /'
        fi
    else
        fail "prior gate missing or not executable: $GATE"
    fi
done

# ---------------------------------------------------------------------------
# Section 12 — Cloud pytest regression (no cloud changes this prompt).
# ---------------------------------------------------------------------------
section "12. Cloud pytest regression"

if [ "${SKIP_PYTEST:-0}" = "1" ]; then
    echo "  SKIP  SKIP_PYTEST=1 set"
elif [ -d "$CLOUD_DIR/tests" ] && command -v python3 >/dev/null 2>&1; then
    if (cd "$CLOUD_DIR" && PYTHONPATH=. python3 -m pytest -x tests/ > /tmp/futuner_ui_pytest.log 2>&1); then
        pass "cloud pytest passed"
        [ "${UI_EVAL_KEEP_LOGS:-0}" != "1" ] && rm -f /tmp/futuner_ui_pytest.log
    else
        fail "cloud pytest FAILED — see /tmp/futuner_ui_pytest.log"
        tail -40 /tmp/futuner_ui_pytest.log | sed 's/^/        /'
    fi
else
    echo "  SKIP  python3 / cloud tests dir unavailable"
fi

# ---------------------------------------------------------------------------
# Section 13 — Full firmware build (idf.py).
# ---------------------------------------------------------------------------
section "13. Full firmware build (idf.py)"

if [ "${SKIP_IDF_BUILD:-0}" = "1" ]; then
    echo "  SKIP  SKIP_IDF_BUILD=1 set"
elif [ -d "${IDF_PATH:-$HOME/esp/esp-idf}" ]; then
    if (cd "$FW_ROOT" && ./build.sh > /tmp/futuner_ui_build.log 2>&1); then
        pass "idf.py build exited 0 (bundle step ran cleanly)"
        [ "${UI_EVAL_KEEP_LOGS:-0}" != "1" ] && rm -f /tmp/futuner_ui_build.log
    else
        fail "idf.py build FAILED — see /tmp/futuner_ui_build.log"
        tail -40 /tmp/futuner_ui_build.log | sed 's/^/        /'
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
echo "All checks green. You may declare Prompt 9 complete."
exit 0
