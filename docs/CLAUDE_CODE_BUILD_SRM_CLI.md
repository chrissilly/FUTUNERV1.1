# Claude Code — Build `tools/srm` CLI

> One Claude Code session builds a unified Python CLI that
> consolidates the bench/cloud/flash/validate workflows. After it
> ships, future Claude Code prompts just say "run `srm <verb>`"
> and the CLI handles everything via env vars + arguments.
>
> No more 100-line bench prompts. The CLI itself encapsulates the
> phase-gated flow.

---

## Paste this into Claude Code

```
Build tools/srm — a unified Python CLI that consolidates the FUTUNER
bench / cloud / flash / validate workflows behind subcommands. Each
subcommand encapsulates one of the multi-phase flows currently
described in separate docs.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_RSYNC_CLOUD.md
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_FLASH_ONLY.md
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_VALIDATE_DONGLE.md
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_FULL.md
- ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_CAPTURE_FLASH.md
- ~/esp/obd/FUTV1.1/docs/upload2server.md
- ~/esp/obd/FUTV1.1/tools/bundle_ui.py    (style reference)
- ~/esp/obd/FUTV1.1/tools/can_sniff.py    (style + Candlelight usage)
- ~/esp/obd/FUTV1.1/firmware/src/commands/commands.c (WS surface)

ABSOLUTE rules carry through:
- No firmware C changes. No frozen-module modifications.
- No cloud server modifications.
- No magic numbers; CONFIG block at top of each module with
  comments + "needs Sean's approval before lock" annotations.
- CAN ID 0x7E0 only.
- Mandatory: append to ~/esp/obd/status-2026-05-07.md and
  ~/esp/obd/file-update-2026-05-07.md.

Pre-decided choices:

Q1 — STRUCTURE: a Python package at tools/srm/ with:
  tools/srm/__init__.py
  tools/srm/__main__.py        — entry point, dispatcher
  tools/srm/cli.py             — argparse setup, subcommand registry
  tools/srm/common.py          — shared utilities (env-var validation,
                                  PASS/FAIL chip printing, status-log
                                  appending, error-halt behavior)
  tools/srm/commands/
    __init__.py
    capture.py                 — MagicMotorsport capture session
    rsync.py                   — Cloud source rsync + Docker rebuild
    flash.py                   — Flash firmware to dongle
    validate.py                — Validate dongle Phase 1 features
    full.py                    — End-to-end (flash + provision +
                                  validate); calls flash + validate
                                  internally
    doctor.py                  — Pre-flight check; verify env vars,
                                  required tools (sshpass, gs_usb,
                                  pyserial, ESP-IDF), reachability
                                  of the dongle and cloud server
    status.py                  — Print current state — what's flashed,
                                  what's enrolled, what's in flight

  tools/srm/wrapper.sh         — bash launcher that adds tools/srm to
                                  PYTHONPATH and dispatches to
                                  __main__.py. Symlinked as
                                  tools/srm so the user types
                                  `tools/srm <verb>`.

Q2 — INVOCATION: hands-off after `tools/srm <verb>`. No interactive
prompts. All inputs come from env vars (read at start; fail fast if
required ones are missing). Required env vars vary by subcommand;
each subcommand declares its own list at the top of its module.

Q3 — EXIT CODES:
  0 = success
  1 = pre-flight failure (missing env var, missing tool, etc.)
  2 = phase failure (rsync failed, build failed, etc.)
  3 = halt-and-surface (gateway lockout, dongle unresponsive,
                        unexpected CAN traffic)
  Each code's meaning is documented in tools/srm/common.py and the
  README.

Q4 — STATUS REPORTING: every subcommand ends with a single block
appended to ~/esp/obd/file-update-YYYY-MM-DD.md, plus a printed
report on stdout. Report format follows the per-prompt format we've
already established (PASS/FAIL chips, anomalies section).

Q5 — TEST LAYER (eval-gated; same shape as prior prompts):
  tools/srm/test/test_cli.py — pytest with mocks. Each subcommand
  tested for:
    - argparse parses correctly
    - --help exits 0
    - --dry-run runs every phase without invoking subprocess or
      hitting network; mock subprocess + requests + websockets
    - missing env var → exit code 1 with clear error
    - phase failure path → exit code 2 with diagnostic preserved
    - halt-and-surface path → exit code 3 with anomaly captured
  Total: at least 9 named test scenarios, listed literally so the
  eval scenario-grep matches.

Q6 — SUBCOMMAND CONTENT: lift the phase logic from the existing
docs (referenced in Read first). Each subcommand's module is the
Python translation of its corresponding doc's prompt body.
For capture.py specifically: include the human-in-loop "press Enter"
prompts for each MagicMotorsport flash cycle. Other subcommands have
zero interactive pauses.

Q7 — FORBIDDEN:
- No firmware/src/* changes
- No cloud/src/* changes
- No ui/* changes
- No new firmware WS commands
- No npm/JS deps
- No hardcoded ADMIN_API_KEY, MAC, or password anywhere
- No commits without Sean's ask

Mocking pattern (tools/srm/test):
- subprocess.run, requests.get/post, websockets.connect mocked at
  the boundary via monkeypatch
- gs_usb (Candlelight) mocked for capture.py tests
- pyserial mocked for flash.py boot-watch tests
- All five existing eval gates plus verify_frozen.sh re-run as
  regression check

Wiring (small focused changes outside tools/srm/):
- tools/README.md (UPDATE if present, CREATE if missing) — document
  the srm CLI entry, list subcommands with one-line summaries,
  pip-deps note
- .gitignore — add tools/srm/__pycache__/, tools/srm/*.pyc

Acceptance criteria — verify ALL before declaring done:
- pytest tools/srm/test/ exits 0 with all 9+ scenarios named and
  passing
- tools/srm --help exits 0 with subcommand list
- tools/srm <verb> --help exits 0 for each verb
- tools/srm <verb> --dry-run for each verb prints the plan,
  zero side effects (mocked subprocess/network)
- pyflakes clean on all tools/srm/*.py
- All SIX prior eval gates still PASS regression-clean
  (feature_manager, wot_logger, dtc, vin_pairing, sbf, ui)
- firmware/test/verify_frozen.sh exits 0
- cloud/tests passes (no cloud changes; regression check)

Hard rules carry through:
- CAN ID 0x7E0 only
- No magic numbers — CONFIG dict per module, annotated
- ON/OFF discipline (each subcommand consumes WS commands that
  arbitrate via feature_manager; no bypass)
- Mandatory progress logs

When done:
1. cd ~/esp/obd/FUTV1.1
2. pytest tools/srm/test/
3. tools/srm --help
4. for verb in capture rsync flash validate full doctor status; do
     tools/srm $verb --help
     tools/srm $verb --dry-run
   done
5. Re-run the six prior gates AND verify_frozen.sh
6. Print the proposed CONFIG defaults table per subcommand
7. Hand back

Ask clarifying questions BEFORE writing code if anything is unclear,
particularly: (a) how subcommands like `full` should compose
(direct function call vs. subprocess vs. import); (b) whether
capture.py should write to firmware/test/can_capture/fixtures/
directly or to a configurable output dir; (c) whether srm validate
should optionally invoke srm capture on failure for diagnostics.

Recommended answers if you have to ask: (a) direct function call
keeps it simple and testable; (b) configurable output dir defaulting
to firmware/test/can_capture/fixtures/magicmotorsport/; (c) NO,
capture is a separate operation with hardware preconditions that
validate doesn't know about.

Proceed.
```

---

## What this gets you, after it lands

Every subsequent operation collapses to one command:

```
tools/srm doctor          # confirm env is set up
tools/srm rsync           # push cloud source + rebuild
tools/srm flash           # build + flash dongle
tools/srm validate        # run all 7 validation phases
tools/srm capture --cycles 5    # MagicMotorsport capture session
tools/srm full            # flash + provision + validate end-to-end
tools/srm status          # show current state
```

No more pasting 100-line prompt bodies for routine operations. The
CLI encapsulates the phase logic; you invoke it. Future Claude Code
prompts (for new feature work) can just say "run `tools/srm
validate` after building" without reimplementing the validation
flow.

Env vars stay the same as the per-doc prompts:
`CLOUD_SSH_PASS`, `ADMIN_API_KEY`, `STA_SSID`, `STA_PASS`,
`DONGLE_HOST`. `tools/srm doctor` checks that everything required
for a given subcommand is set, before you try to run it.

After this lands, the proliferation of separate `CLAUDE_CODE_*.md`
docs becomes obsolete — they collapse into the CLI's `--help`
text. Worth deleting them in a follow-up commit once `srm` is
proven on bench day.
