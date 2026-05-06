# Claude Code — tandem session kickoff (Bench CAN Toolkit)

> Run this in PARALLEL with the firmware Claude Code session that's
> working on Prompt 2 (WOT logger). The two are independent.

---

## Coordination rules (READ FIRST — these prevent the two sessions from colliding)

1. **Use a separate git branch.** This session works on
   `feat/can-capture-toolkit`. Either check out the branch in the
   existing tree, or use a separate worktree at `~/esp/obd-can/FUTV1.1/`.
   Either works. Do not commit to `main`.

2. **Stay in your sandbox.** This session writes ONLY:
   - `firmware/test/can_capture/`  (entire directory — yours)
   - `docs/BENCH_CAN_TOOLKIT.md`   (already exists; you may UPDATE it
     if your implementation requires clarification, but do not rewrite
     wholesale; the firmware sessions read this doc too)
   - `docs/CLAUDE_CODE_TANDEM_CAN.md` (this file — do NOT modify)

   Forbidden directories (the eval harness will catch violations):
   - `firmware/src/` — the other session is in here
   - `firmware/main/`, `firmware/components/`, `firmware/include/`,
     `firmware/lib/`
   - `firmware/test/feature_manager/` — already shipped, do not touch
   - `firmware/test/test_feature_manager.c` — same
   - `cloud/`, `ui/`, `secrets/`, `sbf/`, `tools/`

3. **Use a separate status log.** Write to:
   - `~/esp/obd/status-2026-05-05-can.md`
   - `~/esp/obd/file-update-2026-05-05-can.md`

   The firmware session writes to the unsuffixed names. Sean merges
   them manually.

4. **No firmware build.** This session does not need ESP-IDF or any
   firmware compile. The eval harness has `SKIP_IDF_BUILD=1` baked in
   conceptually — you don't run `idf.py` at all.

5. **No hardware.** This session never touches a real Candlelight, a
   real CAN bus, or a real dongle. All grading is against synthetic
   fixtures. Hardware-in-the-loop validation is a separate, later
   step that Sean does manually after this session's eval is green.

---

## Paste this prompt into the tandem Claude Code session

```
You are a tandem Claude Code session running in parallel with another
Claude Code session that is currently building the WOT logger (Prompt 2)
in firmware/src/ and firmware/test/. Your work and theirs MUST NOT
collide. You operate in a strict sandbox.

Read these files in this order before doing anything else:

1. ~/esp/obd/CLAUDE.md                                  (workspace router)
2. ~/esp/obd/FUTV1.1/CLAUDE.md                          (project rules)
3. ~/esp/obd/FUTV1.1/docs/BENCH_CAN_TOOLKIT.md          (YOUR spec — every detail here is binding)
4. ~/esp/obd/FUTV1.1/docs/CLAUDE_CODE_TANDEM_CAN.md     (the coordination rules)
5. ~/esp/obd/FUTV1.1/firmware/src/FROZEN_MODULES.md     (what you cannot touch)
6. ~/esp/obd/FUTV1.1/firmware/test/can_capture/eval.sh  (the gate)
7. ~/esp/obd/FUTV1.1/firmware/test/can_capture/fixtures/read_vin.notes.md
   (the starter fixture you must satisfy)

Your job: build the bench CAN toolkit per the spec, in
~/esp/obd/FUTV1.1/firmware/test/can_capture/. Nothing outside that
directory except possibly clarifying edits to docs/BENCH_CAN_TOOLKIT.md.

Pre-decided technical choices (do not re-litigate; the firmware session
is using its own clarification budget, you should use yours sparingly):

- Language for parser: Python 3.10+. Test framework: pytest. Type hints
  required, mypy-clean recommended.
- CAN backend: Linux SocketCAN preferred. Must work on macOS via
  python-can with the gs_usb backend, but full Linux SocketCAN
  capability is the primary target. If a feature is Linux-only,
  document it in README and exit cleanly with a clear message on macOS.
- ISO-TP reassembly: kernel `isotp` module (Linux) when available, with
  python-can-isotp as fallback. Detect at runtime; do not hardcode.
- Output format: one JSON object per UDS message, JSONL, exact schema
  per BENCH_CAN_TOOLKIT.md §"Toolkit API → Parse".
- Capture format: candump -tz -L (timestamps + log format). The kernel
  candump tool is the authoritative format; do not invent a new one.
- Safety: cansend_safe.sh refuses tx unless first two args are EXACTLY
  --allow-tx --target=bench. There is no --target=car option. Audit
  log every tx to tx_log/<ts>.tx.log.

Files to create (each does ONE thing — keep them short and focused):

- firmware/test/can_capture/README.md
    Short — points at docs/BENCH_CAN_TOOLKIT.md as the canonical spec.
    Documents prerequisites (Linux + SocketCAN tools, or macOS +
    python-can + libusb), how to install (pip install -r requirements.txt),
    how to run a sample capture-and-parse cycle.

- firmware/test/can_capture/requirements.txt
    Pinned Python deps. At minimum: python-can, python-can-isotp,
    pytest. Pin versions; do not use bare "python-can".

- firmware/test/can_capture/bench/can_setup.sh
- firmware/test/can_capture/bench/can_teardown.sh
- firmware/test/can_capture/bench/capture_start.sh
- firmware/test/can_capture/bench/capture_stop.sh
- firmware/test/can_capture/bench/cansend_safe.sh
    Per BENCH_CAN_TOOLKIT.md §"Toolkit API". Each does one thing,
    documented argument set, exits with clear status codes.

- firmware/test/can_capture/bench/parse_uds.py
    The parser. Reads candump log, reassembles ISO-TP, decodes UDS
    services, emits JSONL per the schema. Module-style — should also
    be importable and have an internal pytest suite under tests/.

- firmware/test/can_capture/tests/test_parse_uds.py
    pytest suite for the parser. Independent of the eval harness;
    invoked via `pytest firmware/test/can_capture/tests/`.

- firmware/test/can_capture/fixtures/<more>.candump,
  fixtures/<more>.expected.jsonl, fixtures/<more>.notes.md
    Add at least 2 more fixture trios beyond the starter read_vin one
    so the parser is exercised on more than the trivial path. Suggested
    additions:
      * negative_response — UDS 0x7F (negative response) sequence.
      * multi_did_read — ReadDataByIdentifier with multiple DIDs in
                         one request (tests parser's multi-DID path).
    Use realistic UDS frames per the documented services in
    ~/esp/obd/FUTV1.1/docs/CAN_UDS_PROTOCOL.md.

Hard constraints:

- The starter fixture at fixtures/read_vin.{candump,expected.jsonl,notes.md}
  exists already. Your parser MUST produce the expected output for it,
  byte-exact (modulo the ts field which the eval normalizes to "TS").
  If the expected output looks wrong to you, DO NOT silently change
  the .expected.jsonl — that defeats the contract. Stop and surface
  the specific concern as a question.

- No code outside the sandbox. The eval harness checks this with git
  status; out-of-sandbox modifications fail the harness regardless of
  intent.

- No transmission paths anywhere except cansend_safe.sh. The parser
  doesn't transmit. The capture scripts don't transmit. Only the one
  documented script does, and only with the right flags.

- No magic numbers. Bitrate, default interface name, capture max
  duration, etc. — all configurable via flags or a config file
  (firmware/test/can_capture/bench/defaults.cfg or similar). Document
  proposed defaults as needing Sean's approval.

When done — gate sequence:

1. cd ~/esp/obd/FUTV1.1
2. firmware/test/can_capture/eval.sh
3. If RESULT: PASS — print full output, write to status-2026-05-05-can.md
   and file-update-2026-05-05-can.md, hand back. Note in your summary
   that hardware-in-the-loop validation is the next step Sean does
   manually with a real Candlelight.
4. If RESULT: FAIL — fix and re-run. Do not declare done with any FAIL.

Ask clarifying questions BEFORE starting if anything is ambiguous in
the spec, the fixture, or the eval harness contract. Do not assume.

Proceed.
```

---

## What this session does NOT do

- It does not flash any firmware (no ESP-IDF, no idf.py, no USB).
- It does not test against real hardware (synthetic fixtures only).
- It does not interact with the dongle, the dev car, the cloud server,
  or any feature-level code.
- It does not orchestrate end-to-end iteration loops (that's a later
  per-feature concern; this session just builds the toolkit primitives).

## What success looks like

`firmware/test/can_capture/eval.sh` exits 0. Toolkit is ready for
hardware-in-the-loop validation. When Sean later plugs in a real
Candlelight, the toolkit's `can_setup.sh` brings up the interface, a
capture round-trips through `capture_start.sh` → real bus traffic →
`capture_stop.sh` → `parse_uds.py` and produces sensible JSONL.
