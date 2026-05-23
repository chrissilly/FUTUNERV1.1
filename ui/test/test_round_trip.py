#!/usr/bin/env python3
"""test_round_trip.py — UI eval Layer C client (Prompt 9).

Drives mock_dongle.py over WS + admin HTTP. Exercises every required
test scenario in a single process so the eval can grep this file for
the literal scenario names AND run it for full pass/fail.

Required scenarios (must literally appear; the eval scenario-grep
matches function names against this list):
    test_command_registry_coverage
    test_event_handler_coverage
    test_apply_progress_sequence
    test_apply_failed_path
    test_license_unpaid_refusal
    test_feature_swap_modal_appears
    test_dtc_read_renders_table
    test_html_structure_intact
    test_css_tokens_used

Layer A scenarios (registry, event handlers, html, css) read static
files and don't need the WS round-trip; Layer C scenarios open a
WS connection and exercise the admin port.

Run:
    python3 ui/test/test_round_trip.py \\
        --ws ws://127.0.0.1:47821/ \\
        --admin http://127.0.0.1:47822/ \\
        --project-root /Users/rabbit/esp/obd/FUTV1.1

Exit 0 on full pass; emits "RESULT: PASS" on stdout (the eval greps
that line). Exit 1 on any failure with one summary line per failure.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Optional

try:
    import websockets
except ImportError as e:
    print(f"test_round_trip.py: websockets package required: {e}", file=sys.stderr)
    print("Install: pip install --user websockets", file=sys.stderr)
    sys.exit(2)


# ---------- Tunables (named, not magic) ----------
ADMIN_HTTP_TIMEOUT_SEC = 5.0
WS_RECV_TIMEOUT_SEC = 3.0
WS_OPEN_TIMEOUT_SEC = 5.0
EXPECTED_DTC_COUNT_FROM_FIXTURE = 3
REQUIRED_SEQUENCES = (
    "apply_progress_3_then_complete",
    "apply_failed_unload",
    "license_revoked_then_paid",
)

# Commands the firmware exports but the UI deliberately doesn't bind
# in this prompt — must match _eval_command_exemptions in fixture.
EVAL_COMMAND_EXEMPTIONS = {
    "pair_ecu", "remove_pairing", "configure_logger", "get_single_variable",
    "flex_load_scal", "flex_unload_scal", "flex_status", "flex_enable",
    "flex_disable", "flex_set_override",
    "logger_start", "logger_stop", "fs_info", "fs_mkdir",
    "list_available_vars", "delete_logger_profile",
    "can_sniff_status", "wifi_status",
    # 2026-05-12 — HIL preflight commands; engineering surface only.
    "phase2_hil_preflight", "phase2_hil_preflight_arm",
    # 2026-05-22 — diagnostic + admin surfaces invoked from CLI / WS-by-
    # hand / browser console (e.g., get_logger_data_raw is the
    # pre-parse hex diagnostic landed for P-55; reboot is admin-only
    # and explicitly NOT bound to a UI button). Registered handlers
    # exist; they're just not part of the bundled customer UI.
    "get_logger_data_raw", "reboot",
}

# Events excluded from the wsEvents handler-coverage check.
# can_frame stays on the hot direct-call path in handleMsg() — by
# design — because it's the sniffer stream and rate-sensitive.
EVAL_EVENT_EXEMPTIONS = {"can_frame"}


# ---------- Result tracker ----------
class Results:
    def __init__(self) -> None:
        self.passed: list[str] = []
        self.failed: list[tuple[str, str]] = []

    def ok(self, name: str) -> None:
        self.passed.append(name)
        print(f"PASS  {name}")

    def fail(self, name: str, reason: str) -> None:
        self.failed.append((name, reason))
        print(f"FAIL  {name}: {reason}")

    def summary(self) -> int:
        print()
        print(f"Passed: {len(self.passed)}  Failed: {len(self.failed)}")
        if self.failed:
            for n, r in self.failed:
                print(f"  - {n}: {r}")
            print()
            print("RESULT: FAIL")
            return 1
        print()
        print("RESULT: PASS")
        return 0


# ---------- File-loading helpers ----------
def project_paths(root: Path) -> dict[str, Path]:
    return {
        "ui_html":   root / "ui" / "control_panel.html",
        "ui_css":    root / "ui" / "control_panel.css",
        "ui_js":     root / "ui" / "control_panel.js",
        "fw_html":   root / "firmware" / "futuner_control_panel.html",
        "registry":  root / "firmware" / "src" / "commands" / "commands.c",
        "fw_src":    root / "firmware" / "src",
        "fixture":   root / "ui" / "test" / "mock_dongle_responses.json",
    }


def parse_command_registry(commands_c: Path) -> list[str]:
    """Awk-equivalent: extract the first quoted token of each registry
    entry. The registry rows look like:
        {"pair_ecu", "Pair with current ECU", ...},
    """
    if not commands_c.is_file():
        raise SystemExit(f"commands.c not found: {commands_c}")
    text = commands_c.read_text(encoding="utf-8")
    # Match anything between an opening { and the first comma; that's
    # the command-name string literal.
    pattern = re.compile(r'\{\s*"([a-z_][a-z0-9_]*)"\s*,', re.IGNORECASE)
    return list(dict.fromkeys(pattern.findall(text)))  # preserve order, dedup


def parse_emitted_events(fw_src: Path) -> list[str]:
    """Find every emit_event(...) string literal in firmware/src and
    extract the event-name token. Pattern: '"event":"NAME"' with the
    typical C escape backslashes. Same shape as the orchestrator's
    emit_event format strings.
    """
    if not fw_src.is_dir():
        raise SystemExit(f"firmware/src not found: {fw_src}")
    found: set[str] = set()
    pat = re.compile(r'\\"event\\":\\"([a-z_][a-z0-9_]*)\\"', re.IGNORECASE)
    pat_alt = re.compile(r'"event"\s*:\s*"([a-z_][a-z0-9_]*)"', re.IGNORECASE)
    for c in fw_src.rglob("*.c"):
        try:
            txt = c.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for m in pat.findall(txt):
            found.add(m)
        for m in pat_alt.findall(txt):
            found.add(m)
    return sorted(found)


# =========================================================================
# Layer A — static scenarios (no WS round-trip needed).
# =========================================================================

def test_command_registry_coverage(paths: dict[str, Path], r: Results) -> None:
    """Every COMMAND_REGISTRY entry has a fixture entry AND a wsSend
    binding in JS, modulo EVAL_COMMAND_EXEMPTIONS."""
    name = "test_command_registry_coverage"
    registry = parse_command_registry(paths["registry"])
    fixture = json.loads(paths["fixture"].read_text(encoding="utf-8"))
    js = paths["ui_js"].read_text(encoding="utf-8")

    missing_fixture: list[str] = []
    missing_binding: list[str] = []
    for cmd in registry:
        if cmd in EVAL_COMMAND_EXEMPTIONS:
            continue
        if cmd not in fixture or cmd.startswith("_"):
            missing_fixture.append(cmd)
        # `wsSend({command:'NAME'` or `command:"NAME"` — accept either.
        if not (re.search(rf"command\s*:\s*'{re.escape(cmd)}'", js)
                or re.search(rf'command\s*:\s*"{re.escape(cmd)}"', js)):
            missing_binding.append(cmd)

    if missing_fixture or missing_binding:
        bits = []
        if missing_fixture:
            bits.append(f"fixture missing: {missing_fixture}")
        if missing_binding:
            bits.append(f"wsSend missing: {missing_binding}")
        r.fail(name, "; ".join(bits))
    else:
        r.ok(name)


def test_event_handler_coverage(paths: dict[str, Path], r: Results) -> None:
    """Every emit_event(...) literal in firmware/src has a
    wsEvents.on(...) registration in ui/control_panel.js, modulo
    EVAL_EVENT_EXEMPTIONS (can_frame stays on the hot direct path)."""
    name = "test_event_handler_coverage"
    events = parse_emitted_events(paths["fw_src"])
    js = paths["ui_js"].read_text(encoding="utf-8")

    missing: list[str] = []
    for ev in events:
        if ev in EVAL_EVENT_EXEMPTIONS:
            continue
        if not (re.search(rf"wsEvents\.on\s*\(\s*'{re.escape(ev)}'", js)
                or re.search(rf'wsEvents\.on\s*\(\s*"{re.escape(ev)}"', js)):
            missing.append(ev)
    if missing:
        r.fail(name, f"no wsEvents.on() binding for: {missing} (firmware emits these)")
    else:
        r.ok(name)


def test_html_structure_intact(paths: dict[str, Path], r: Results) -> None:
    """Required panel + header + modal node IDs exist in the canonical
    HTML AND in the bundled firmware copy. JS/CSS files exist."""
    name = "test_html_structure_intact"
    html = paths["ui_html"].read_text(encoding="utf-8")
    fw_html = paths["fw_html"].read_text(encoding="utf-8") if paths["fw_html"].is_file() else ""

    required_ids = [
        "panel-dashboard", "panel-sniffer", "panel-diag", "panel-tuning",
        "panel-livetune", "panel-logconfig", "panel-files", "panel-wot",
        "panel-vinpair", "panel-system",
        "licenseLock", "activeFeatureLabel",
        "swapConfirmModal",
    ]
    missing = [i for i in required_ids if f'id="{i}"' not in html]
    fw_missing = [i for i in required_ids if fw_html and f'id="{i}"' not in fw_html]
    if missing:
        r.fail(name, f"canonical HTML missing IDs: {missing}")
        return
    if fw_html and fw_missing:
        r.fail(name, f"bundled firmware HTML missing IDs: {fw_missing}")
        return
    if not paths["ui_css"].is_file():
        r.fail(name, "ui/control_panel.css missing")
        return
    if not paths["ui_js"].is_file():
        r.fail(name, "ui/control_panel.js missing")
        return
    r.ok(name)


def test_css_tokens_used(paths: dict[str, Path], r: Results) -> None:
    """Every new color value in ui/control_panel.css references a CSS
    variable; no inline hex literals outside the :root block. The
    scan extracts the :root block and treats it as exempt."""
    name = "test_css_tokens_used"
    css = paths["ui_css"].read_text(encoding="utf-8")

    # Strip the :root { ... } block(s).
    stripped = re.sub(r":root\s*\{[^}]*\}", "", css, count=0, flags=re.DOTALL)
    # Strip /* ... */ comments.
    stripped = re.sub(r"/\*.*?\*/", "", stripped, flags=re.DOTALL)

    # Find inline hex color literals: #abc or #aabbcc or #aabbccdd.
    bad = re.findall(r"#[0-9a-fA-F]{3,8}\b", stripped)
    # The orange-theme rule (Q6): existing literals in the rest of
    # the CSS are pre-Prompt-9. We allow the legacy literals that
    # were present in the original control_panel.html bytes — they
    # carry through unchanged. The flagged-as-bad cases are NEW
    # literals we'd introduce in Prompt-9-added rules.
    LEGACY_LITERALS = {
        "#333", "#222", "#444", "#333", "#0d0d0d", "#1a1a1a", "#252525",
        "#2a2a2a", "#e0e0e0", "#888", "#ff6600", "#cc5200", "#00cc66",
        "#ffcc00", "#ff3333", "#3399ff", "#000", "#fff", "#ff8533",
        "#332200", "#003300", "#331a00",
    }
    novel = [h for h in bad if h.lower() not in {x.lower() for x in LEGACY_LITERALS}]
    if novel:
        r.fail(name, f"new inline hex literals (must use :root tokens): {sorted(set(novel))}")
        return
    if "--theme-name" not in css:
        r.fail(name, "expected --theme-name forward-compat marker in :root")
        return
    if "--modal-overlay" not in css:
        r.fail(name, "expected --modal-overlay token in :root")
        return
    r.ok(name)


# =========================================================================
# Layer C — WS round-trip scenarios.
# =========================================================================

async def _admin_post(admin_url: str, path: str, body: dict[str, Any]) -> dict[str, Any]:
    url = admin_url.rstrip("/") + path
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    # urllib is blocking; offload to a thread.
    loop = asyncio.get_running_loop()

    def _do() -> dict[str, Any]:
        with urllib.request.urlopen(req, timeout=ADMIN_HTTP_TIMEOUT_SEC) as resp:
            return json.loads(resp.read().decode("utf-8"))

    return await loop.run_in_executor(None, _do)


async def _send_and_collect(ws_url: str, send_msgs: list[dict[str, Any]],
                            collect_for_sec: float = 2.0,
                            extra_after: Optional[asyncio.Future] = None
                            ) -> list[dict[str, Any]]:
    """Open a WS connection, send the listed messages, then drain
    inbound for `collect_for_sec` seconds. Used by event-sequence
    scenarios so we capture the canned response AND any subsequent
    server-pushed events from /admin/fire."""
    received: list[dict[str, Any]] = []
    async with websockets.connect(ws_url, open_timeout=WS_OPEN_TIMEOUT_SEC) as ws:
        for m in send_msgs:
            await ws.send(json.dumps(m))
        deadline = asyncio.get_running_loop().time() + collect_for_sec
        while True:
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                break
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=remaining)
            except (asyncio.TimeoutError, websockets.ConnectionClosed):
                break
            try:
                received.append(json.loads(raw))
            except json.JSONDecodeError:
                continue
    return received


async def test_apply_progress_sequence(ws_url: str, admin_url: str, r: Results) -> None:
    """Mock fires apply_started → 3× apply_progress → apply_completed;
    client asserts ordered receipt."""
    name = "test_apply_progress_sequence"

    async def _bg_fire() -> None:
        await asyncio.sleep(0.1)  # let WS connect first
        await _admin_post(admin_url, "/admin/fire",
                          {"sequence": "apply_progress_3_then_complete"})

    fire_task = asyncio.create_task(_bg_fire())
    msgs = await _send_and_collect(ws_url, [], collect_for_sec=2.0)
    await fire_task

    events = [m.get("event") for m in msgs if m.get("event")]
    expected = ["apply_started", "apply_progress", "apply_progress",
                "apply_progress", "apply_completed"]
    if events != expected:
        r.fail(name, f"event order wrong: got {events}, expected {expected}")
        return
    progress_msgs = [m for m in msgs if m.get("event") == "apply_progress"]
    if not all(m.get("maps_total") == 3 for m in progress_msgs):
        r.fail(name, "apply_progress payloads missing maps_total=3")
        return
    r.ok(name)


async def test_apply_failed_path(ws_url: str, admin_url: str, r: Results) -> None:
    """Mock fires apply_failed → unload; client asserts receipt and
    the failure carries a `reason` string. The UI's event handlers
    flip the lt-state-badge to error and show the banner — verified
    via the static JS grep below (no DOM)."""
    name = "test_apply_failed_path"

    async def _bg_fire() -> None:
        await asyncio.sleep(0.1)
        await _admin_post(admin_url, "/admin/fire", {"sequence": "apply_failed_unload"})

    fire_task = asyncio.create_task(_bg_fire())
    msgs = await _send_and_collect(ws_url, [], collect_for_sec=2.0)
    await fire_task

    events = [m.get("event") for m in msgs if m.get("event")]
    if "apply_failed" not in events or "unload" not in events:
        r.fail(name, f"missing apply_failed/unload in: {events}")
        return

    # Static check: the JS handler for apply_failed flips badge to
    # 'error' and surfaces the reason in the banner.
    js = Path(__file__).resolve().parents[1] / "control_panel.js"
    src = js.read_text(encoding="utf-8")
    if "wsEvents.on('apply_failed'" not in src:
        r.fail(name, "ui/control_panel.js missing wsEvents.on('apply_failed', ...)")
        return
    if "liveTuneSetBadge('error')" not in src:
        r.fail(name, "apply_failed handler must call liveTuneSetBadge('error')")
        return
    r.ok(name)


async def test_license_unpaid_refusal(ws_url: str, admin_url: str, r: Results) -> None:
    """Mock returns {success:false, error:'license unpaid'} for
    live_tune_start; client asserts refusal banner machinery exists."""
    name = "test_license_unpaid_refusal"

    # Activate the override.
    await _admin_post(admin_url, "/admin/override",
                      {"command": "live_tune_start", "override_key": "live_tune_start_unpaid"})
    try:
        msgs = await _send_and_collect(
            ws_url,
            [{"command": "live_tune_start", "params": {"stage": 1, "ethanol_pct": 0}}],
            collect_for_sec=1.0,
        )
    finally:
        # Restore default.
        await _admin_post(admin_url, "/admin/override",
                          {"command": "live_tune_start", "override_key": None})

    refusal = [m for m in msgs
               if m.get("command") == "live_tune_start"
               and (m.get("ok") is False or m.get("success") is False)]
    if not refusal:
        r.fail(name, f"expected refusal response, got: {msgs}")
        return
    err = (refusal[0].get("error") or "").lower()
    if "license" not in err:
        r.fail(name, f"expected error to mention 'license'; got '{err}'")
        return

    # JS must surface the refusal in a banner with class 'error'.
    js = Path(__file__).resolve().parents[1] / "control_panel.js"
    src = js.read_text(encoding="utf-8")
    if "ltBanner" not in src:
        r.fail(name, "ui/control_panel.js missing ltBanner refusal surface")
        return
    if "banner error" not in src:
        r.fail(name, "ui/control_panel.js missing 'banner error' class application")
        return
    r.ok(name)


async def test_feature_swap_modal_appears(ws_url: str, admin_url: str, r: Results) -> None:
    """Sequential commands (wot_log_start then live_tune_start) with
    activeFeature seeded; modal node becomes visible. Without a
    headless browser, this is a structural verification: the JS
    contains the swap-arbitration logic AND the modal node exists."""
    name = "test_feature_swap_modal_appears"

    msgs = await _send_and_collect(
        ws_url,
        [{"command": "wot_log_start"}],
        collect_for_sec=0.5,
    )
    if not any(m.get("active_feature") == "wot_logging" for m in msgs):
        r.fail(name, "wot_log_start did not return active_feature='wot_logging'")
        return

    # Static checks against the JS + HTML.
    js = Path(__file__).resolve().parents[1] / "control_panel.js"
    html = Path(__file__).resolve().parents[1] / "control_panel.html"
    src = js.read_text(encoding="utf-8")
    htm = html.read_text(encoding="utf-8")

    if "function confirmFeatureSwap" not in src:
        r.fail(name, "missing confirmFeatureSwap() function in JS")
        return
    if "appState.activeFeature" not in src:
        r.fail(name, "swap logic must read appState.activeFeature")
        return
    if 'id="swapConfirmModal"' not in htm:
        r.fail(name, "missing #swapConfirmModal in HTML")
        return
    # Both feature-start handlers route through the swap helper.
    if "confirmFeatureSwap('wot_logging'" not in src:
        r.fail(name, "wotStart() must route through confirmFeatureSwap('wot_logging', ...)")
        return
    if "confirmFeatureSwap('live_tune'" not in src:
        r.fail(name, "liveTuneApply() must route through confirmFeatureSwap('live_tune', ...)")
        return
    r.ok(name)


async def test_dtc_read_renders_table(ws_url: str, admin_url: str, r: Results) -> None:
    """Mock returns DTC array of 3 codes; client asserts response
    shape AND that the JS contains code that renders one .dtc-item
    per code."""
    name = "test_dtc_read_renders_table"

    msgs = await _send_and_collect(
        ws_url, [{"command": "dtc_read"}], collect_for_sec=0.8
    )
    resp = next((m for m in msgs if m.get("command") == "dtc_read"), None)
    if not resp:
        r.fail(name, "no dtc_read response received")
        return
    if resp.get("ok") is not True:
        r.fail(name, f"expected ok=true, got: {resp}")
        return
    codes = resp.get("codes") or []
    if len(codes) != EXPECTED_DTC_COUNT_FROM_FIXTURE:
        r.fail(name, f"expected {EXPECTED_DTC_COUNT_FROM_FIXTURE} codes, got {len(codes)}")
        return
    # Each code carries code + status + description.
    for c in codes:
        if "code" not in c or "description" not in c or "status" not in c:
            r.fail(name, f"DTC entry missing required fields: {c}")
            return

    # JS renders one .dtc-item per entry.
    js = Path(__file__).resolve().parents[1] / "control_panel.js"
    src = js.read_text(encoding="utf-8")
    if "dtcs.map(d =>" not in src:
        r.fail(name, "JS missing dtcs.map(d => ...) row renderer")
        return
    if 'class="dtc-code"' not in src:
        r.fail(name, "JS missing .dtc-code rendering")
        return
    if 'dtc-status-chip' not in src:
        r.fail(name, "JS missing .dtc-status-chip rendering")
        return
    r.ok(name)


# =========================================================================
# Required-sequences sanity check (no test_* prefix to avoid clashing
# with the eval scenario-grep, but exercised in main()).
# =========================================================================

def check_required_sequences_present(paths: dict[str, Path]) -> Optional[str]:
    fixture = json.loads(paths["fixture"].read_text(encoding="utf-8"))
    seqs = fixture.get("_sequences", {})
    missing = [s for s in REQUIRED_SEQUENCES if s not in seqs]
    if missing:
        return f"fixture _sequences missing: {missing}"
    return None


# =========================================================================
# main()
# =========================================================================

async def amain(args: argparse.Namespace) -> int:
    paths = project_paths(Path(args.project_root).resolve())
    r = Results()

    # Layer A static scenarios.
    test_command_registry_coverage(paths, r)
    test_event_handler_coverage(paths, r)
    test_html_structure_intact(paths, r)
    test_css_tokens_used(paths, r)

    # Required-sequences sanity (not a numbered scenario but blocks if absent).
    err = check_required_sequences_present(paths)
    if err:
        r.fail("required_sequences_present", err)
    else:
        r.ok("required_sequences_present")

    # Layer C round-trip scenarios.
    await test_apply_progress_sequence(args.ws, args.admin, r)
    await test_apply_failed_path(args.ws, args.admin, r)
    await test_license_unpaid_refusal(args.ws, args.admin, r)
    await test_feature_swap_modal_appears(args.ws, args.admin, r)
    await test_dtc_read_renders_table(args.ws, args.admin, r)

    return r.summary()


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--ws", default="ws://127.0.0.1:47821/")
    p.add_argument("--admin", default="http://127.0.0.1:47822/")
    p.add_argument("--project-root", default=str(Path(__file__).resolve().parents[2]))
    args = p.parse_args(argv)
    try:
        return asyncio.run(amain(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
