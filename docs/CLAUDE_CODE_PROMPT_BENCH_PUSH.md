# Claude Code — Bench Push Tool (firmware flash + cloud assets + dongle provisioning)

> Paste-ready prompt for a focused tooling session. Single new
> deliverable: `tools/bench_push.py`, a phase-gated CLI that handles
> the four things that have to happen on bench day to put a dongle in
> a usable state — build, flash, push cloud assets, provision the
> device. No new firmware C, no new feature; this is the dev-loop
> wrapper around steps that exist today as separate manual commands
> in `docs/upload2server.md`.
>
> Branch convention: `feat/tools-bench-push`. After eval green, merge
> to main. The eval is light — this is a tool, not a feature — but it
> still gates: argparse parses, dry-run mode prints the plan without
> side effects, idempotency check (running the same phase twice is
> safe).

---

## Paste this into Claude Code

```
Tooling prompt — write tools/bench_push.py, a phase-gated CLI that
unifies the bench-day push pipeline (firmware build + flash + cloud
asset upload + dongle provisioning) into one orchestrator. Pure dev
tool; no firmware C changes, no feature work.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/docs/SESSION_HANDOFF.md
- ~/esp/obd/FUTV1.1/docs/upload2server.md       (the manual procedure
                                                  this script automates)
- ~/esp/obd/FUTV1.1/firmware/build.sh
- ~/esp/obd/FUTV1.1/firmware/flash.sh
- ~/esp/obd/FUTV1.1/firmware/monitor.sh
- ~/esp/obd/FUTV1.1/cloud/src/main.py            (admin endpoints —
                                                  /admin/devices,
                                                  /admin/calibrations,
                                                  /admin/firmware,
                                                  /admin/devices/<mac>/license)
- ~/esp/obd/FUTV1.1/firmware/src/commands/commands.c (WS commands the
                                                       provisioning phase
                                                       fires)
- ~/esp/obd/FUTV1.1/tools/                        (existing tools dir;
                                                   bundle_ui.py, sbf_to_json.py,
                                                   can_sniff.py — match style)

ABSOLUTE rules carry through:
- No firmware C source changes. The script invokes existing scripts
  (./build.sh, ./flash.sh) and existing endpoints; it does not modify
  firmware.
- No frozen-module modifications.
- No cloud server modifications (the script consumes existing admin
  endpoints — adding endpoints is out of scope).
- No magic numbers. CLI defaults (port numbers, timeouts, retry counts,
  paths) live in a named CONFIG dict at the top of the script with
  per-key comments explaining each. Same "Proposed default — needs
  Sean's approval before lock" annotation pattern.
- CAN ID 0x7E0 only. (Stated reflexively — script doesn't talk CAN
  but the dongle it provisions does.)

Pre-decided choices (Q1–Q6):

Q1 — LANGUAGE + LOCATION: Python 3 stdlib + `requests` + `websockets`.
  - Path: tools/bench_push.py.
  - Stdlib only where possible; allow `requests` and `websockets`
    (already used by ui/test/mock_dongle.py — same pip footprint).
  - Document deps in tools/README.md (CREATE if missing).
  - Shebang: `#!/usr/bin/env python3`. Executable bit set.

Q2 — PHASE STRUCTURE: four phases, each opt-in via CLI flag, all-on
by default.
  - --build       → run firmware/build.sh (which itself runs
                    tools/bundle_ui.py first). Default ON.
  - --flash PORT  → run firmware/flash.sh -p PORT. PORT required if
                    flag present. If PORT omitted, auto-detect
                    /dev/cu.usbmodem* on macOS or /dev/ttyUSB* on
                    Linux; if multiple, error and require explicit -p.
                    Default ON when --flash with auto-detect succeeds;
                    skipped (with a notice) if auto-detect ambiguous.
  - --cloud       → push cloud assets (firmware bin to /admin/firmware,
                    SBF to /admin/calibrations, optional assignment).
                    Default ON.
  - --provision   → finalize device on cloud + WS provisioning to the
                    flashed dongle. Default ON only if --flash also ON
                    (provisioning a device that wasn't just flashed
                    isn't safe — its state is unknown).
  - --all (alias for the four above, explicit form).
  - --only=PHASE  → run exactly one phase, skip the rest. Useful for
                    "I just want to push a new SBF without reflashing."

Q3 — CONFIG / SECRETS: env vars + per-run flags. NO secrets in
files committed to the repo.
  - Required env vars: ADMIN_API_KEY (admin auth), CLOUD_URL
    (default https://sillyrabbitmotorsport.com/fut).
  - Per-run flags: --mac (dongle MAC, required when running --cloud
    or --provision; auto-extracted from boot log when --flash also ON),
    --vin (optional; otherwise dongle reports it on first register),
    --sbf-file (path to SBF to push; optional — if omitted, --cloud
    skips SBF upload), --paid 0|1 (default 1; sets license state),
    --dongle-host (the dongle's WS URL during provision; default
    http://192.168.4.1 — AP mode after fresh flash).
  - At startup, validate required env+flags for the phases requested.
    Fail fast with a helpful error listing what's missing.

Q4 — DRY RUN + LOGGING:
  - --dry-run prints every command/HTTP/WS call that would happen,
    in order, with redacted secrets. No side effects. Always available.
  - --verbose toggles per-step output detail (curl-style HTTP body
    logs in particular).
  - Default output: one-line-per-step status with PASS/FAIL/SKIP
    chips, summary table at the end. Like an eval harness output.
  - Every run appends a timestamped block to
    ~/esp/obd/file-update-YYYY-MM-DD.md describing what was done.
    Mandatory progress-logging rule (CLAUDE.md hard rule #4).

Q5 — IDEMPOTENCY:
  - --build is naturally idempotent (rebuilds incrementally).
  - --flash is naturally idempotent (overwrites).
  - --cloud:
      • Firmware upload (PUT /admin/firmware/<hash>): server uses
        INSERT OR REPLACE; safe to re-run.
      • SBF upload (POST /admin/calibrations/<filename>): server uses
        INSERT OR REPLACE; safe to re-run.
      • Device enroll (POST /admin/devices): server returns 409 on
        duplicate. Script catches 409, treats as "already enrolled,"
        fetches existing token via GET /admin/devices, continues. NOT
        an error.
  - --provision:
      • set_auth_token: WS command; idempotent on the dongle side.
      • wifi_connect: skipped if dongle reports connected already
        (via wifi_status command).
      • vin_pair_now: idempotent — server-side register handles the
        VIN-already-paired-same-VIN case (returns ok:true).
      • License set paid via /admin/devices/<mac>/license: idempotent.
  - Every phase prints "SKIPPED (already in state X)" when applicable
    instead of failing.

Q6 — POST-FLASH BOOT-LOG MAC EXTRACTION:
  When --flash and --provision both ON, the script needs to know the
  dongle's MAC for cloud calls. The boot log emits a line like:
      I (NVS) device MAC: AA:BB:CC:DD:EE:FF
  After flash, the script tails the serial port (via
  firmware/monitor.sh or direct pyserial) for up to 30 s, scrapes the
  MAC line, then proceeds. If --mac is also passed explicitly, the
  scrape result is compared and a mismatch errors out (catches "wrong
  dongle plugged in"). If --mac is not passed and scrape fails (no
  MAC line in 30 s), error with "could not auto-detect MAC; pass
  --mac explicitly."

Q7 — REQUIRED COMMANDS / TEST SCENARIOS (must literally appear in
tools/test_bench_push.py so the eval scenario-grep matches; also
covers the smoke-test layer):
  - test_argparse_help_runs       — `python3 tools/bench_push.py --help`
                                    exits 0 with usage text.
  - test_dry_run_no_side_effects  — --dry-run runs every phase
                                    without invoking subprocess or
                                    making HTTP requests; mock the
                                    `requests` module and assert zero
                                    calls.
  - test_phase_only_isolation     — --only=cloud runs only the cloud
                                    phase; build/flash/provision
                                    handlers never invoked.
  - test_409_enrollment_recovery  — admin/devices returns 409 on a
                                    re-enroll; script falls through
                                    to GET /admin/devices and reuses
                                    the existing token.
  - test_mac_scrape_match         — feed a synthetic boot log line;
                                    script extracts AA:BB:CC:DD:EE:FF.
  - test_mac_scrape_mismatch_errors — --mac AA:BB:CC:DD:EE:FF
                                      conflicts with scraped value;
                                      exit nonzero with clear error.
  - test_missing_env_admin_key    — ADMIN_API_KEY unset + --cloud →
                                    exit nonzero with "set
                                    ADMIN_API_KEY".
  - test_summary_table_renders    — final summary block lists every
                                    requested phase with PASS/FAIL/SKIP.
  - test_log_appended             — script writes a timestamped block
                                    to ~/esp/obd/file-update-YYYY-MM-DD.md.

Q8 — FORBIDDEN LIST FOR THIS PROMPT:
  - Any modification to firmware/src/* or firmware/test/*.
  - Any modification to cloud/src/* or cloud/tests/*.
  - Any modification to ui/* or firmware/futuner_control_panel.html.
  - Any new firmware WS command.
  - Any new cloud HTTP endpoint.
  - Any change to existing tools (bundle_ui.py, sbf_to_json.py,
    can_sniff.py).
  - Adding a JavaScript or TypeScript dep.
  - Hardcoded ADMIN_API_KEY anywhere in the script source or tests.
  - Hardcoded MAC address (the dev-car MAC is per-device — script
    must auto-detect or take as input).

Mocking pattern (tooling test):
  The test layer is light — pytest-style host tests in
  tools/test_bench_push.py. Mock subprocess.run, requests.get/post,
  and websockets.connect at the boundary (function-pointer-injected
  or monkeypatch-style). Exercise the orchestration logic, not the
  external systems.
  Real subprocess invocation would require an actual dongle and an
  actual cloud — out of scope. The eval verifies the orchestrator;
  on-bench manual run verifies the integration.

Wiring (small focused changes outside tools/):
  - tools/README.md (CREATE if missing) — documents the four scripts
    in tools/ (bundle_ui, sbf_to_json, can_sniff, bench_push) with
    a one-paragraph description and pip-deps note.
  - .gitignore — add tools/__pycache__/, tools/*.pyc.

Acceptance criteria — verify ALL before declaring done:
  - tools/test_bench_push.py — pytest exits 0; all 9 required
    scenarios literally present.
  - tools/bench_push.py --help exits 0 with usage text.
  - tools/bench_push.py --dry-run --all (with required env stubbed)
    prints the plan in order without side effects.
  - shellcheck-equivalent (pyflakes or pylint) clean on
    tools/bench_push.py.
  - All SIX prior eval gates still PASS regression-clean
    (feature_manager, wot_logger, dtc, vin_pairing, sbf, ui).
  - firmware/test/verify_frozen.sh exits 0.
  - cloud/tests passes (no cloud changes, regression check).

Hard rules carry through (CLAUDE.md):
  - CAN ID 0x7E0 only.
  - No magic numbers — CONFIG dict at top with comments + approval
    annotations.
  - ON/OFF discipline through feature_manager — script consumes WS
    commands that already arbitrate via feature_manager; doesn't
    bypass.
  - Mandatory: append to ~/esp/obd/status-2026-05-06.md and
    ~/esp/obd/file-update-2026-05-06.md.

When done:
1. cd ~/esp/obd/FUTV1.1
2. tools/test_bench_push.py     (or `pytest tools/test_bench_push.py`)
3. tools/bench_push.py --help
4. tools/bench_push.py --dry-run --all   (with ADMIN_API_KEY stubbed)
5. Re-run the six prior gates AND verify_frozen.sh, confirm
   regression-clean.
6. Print full eval output and the proposed CONFIG defaults table.
   Hand back.

Ask clarifying questions BEFORE writing code if anything is unclear,
particularly around: (a) whether the --provision phase should pause
for user confirmation between irreversible steps (set_auth_token in
particular), (b) whether the boot-log MAC scrape should use
firmware/monitor.sh as a subprocess or direct pyserial (leaning
direct pyserial — fewer moving parts, easier to assert on output),
(c) whether the script should refuse to run if `git status --porcelain`
is non-empty (lean YES — flashing uncommitted work loses the binding
between "what's on the dongle" and "what's in git"; override flag
--allow-dirty for the rare case).

Proceed.
```

---

## Notes for Sean

This is a **tool, not a feature.** The eval gate is correspondingly
light — pytest with mocks, no host_test_runner C compilation, no
idf.py build, no cloud Docker spin-up. The orchestration logic is
what matters; the integration with real dongles and real servers is
validated manually on bench day.

Three scope choices worth flagging:

The **--provision-requires-flash default coupling** (Q2) is the
sharpest end of the script's UX. Provisioning a dongle that wasn't
just flashed is dangerous — the dongle could be in any state
(different VIN cached, different token in NVS, different firmware).
Default-coupling means casual `--provision` calls get blocked
unless you also opted into a flash. Override is `--provision
--no-flash --i-know-what-im-doing` — verbose by design. If the
default feels too paranoid, it's a one-line change in argparse.

The **dirty-tree refuse** (open question (c) above) is the dev-loop
honesty knob. If you flash uncommitted work, "what's on the dongle"
isn't recoverable from git. Recommend the agent default to refusing
on a dirty tree with `--allow-dirty` as the escape hatch. This is
the same discipline the eval gates already encode — frozen modules,
no-magic-numbers, etc. — extended to "no flashing the unknown."
Cheap to enforce, valuable when things go wrong six months later.

The **boot-log MAC scrape** (Q6) is the bit most likely to have a
weird edge case. ESP32-S3 prints the MAC during early boot before
the application banner; if the scrape window is too short or the
serial baud rate is wrong, the script silently fails and falls back
to "could not auto-detect." Recommend the agent test on a real
dongle once during implementation rather than relying on synthetic
boot logs alone — but the agent can't, it's running in a container.
Pragmatic compromise: build the scrape against a captured-from-disk
sample of a real boot log, ship it, and you validate on bench day.
First failure mode is usually "scrape pattern is too strict;" a
one-line regex relaxation fixes it.

After this lands you have a single command for the bench loop:

```bash
ADMIN_API_KEY='...' tools/bench_push.py --all \
    --flash /dev/cu.usbmodem1101 \
    --sbf-file sbf/stage1_patched.sbf \
    --paid 1
```

End-to-end provisioned dongle in one invocation. The four-phase
breakdown means partial loops (just push a new SBF, just reflash,
just re-license) are also one-liners.
