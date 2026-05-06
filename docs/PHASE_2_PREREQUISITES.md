# Phase 2 — Prerequisites Checklist

> Living document. Anything that MUST be true, captured, validated, or
> built before any Phase 2 (full 8 MB ECU binary reflash) code lands
> on a customer device.
>
> Owner: Sean / SRM Engineering. Updated as items close out.
> Created: 2026-05-05.

---

## Status legend

- 🔴 NOT STARTED
- 🟡 IN PROGRESS
- 🟢 DONE

---

## P-01 🔴 MagicMotorsport flash capture session — reference UDS sequences

**Why this is critical.** Phase 2 requires producing a UDS sequence
that an MG1/MDG1 ECU accepts as authoritative. We have AES keys and
research notes, but the only way to be sure FUTUNER's flash code is
correct is to diff it against a known-working reference. MagicMotorsport
is that reference.

**The plan, recorded so it doesn't get lost.**

1. Use the dev RS7 (variant `4K0907557G__0003` per `docs/boxcode_database.md`).
2. Wire the Candlelight USB-CAN sniffer in via Y-splitter on the OBD-II
   port (isolated-ground variant). Dongle does NOT need to be present —
   this is MagicMotorsport doing the talking.
3. Have the MagicMotorsport tool flash the **same binary that's already
   on the ECU**. Re-flashing identical content exercises the full UDS
   challenge-response → security access → encrypted transfer → verify →
   reset pipeline without changing any code on the ECU. Safe by design.
4. Capture 3-5 back-to-back full flash cycles. Multiple cycles are
   important because security access seeds rotate per session — one
   capture would let us accidentally hardcode one seed pair into the
   simulator.
5. For each capture, record on paper or in the notes.md: the boxcode +
   SW number on the ECU at capture time, the ECU's flash-write counter
   value before/after the cycle (visible to dealer tools — note the
   starting value in case it ever matters for warranty/resale), and
   any anomalies during the cycle.

**Where the captures land.**

```
firmware/test/can_capture/fixtures/magicmotorsport/
  flash_run_01_4K0907557G_0003.candump
  flash_run_01_4K0907557G_0003.notes.md
  flash_run_02_4K0907557G_0003.candump
  flash_run_02_4K0907557G_0003.notes.md
  ...
```

**What these captures unlock.**

1. **Parser test fixtures** — bigger, realer, security-access-bearing
   captures than the synthetic ones. The bench CAN toolkit's parser
   gets exercised against real wire traffic.
2. **ECU simulator behavior source** — the `firmware/test/ecu_sim/`
   package (future) replays the ECU's recorded responses so FUTUNER's
   flash code can be developed against a deterministic emulator on the
   bench, not against a live car.
3. **Phase 2 flash-code validation oracle** — when FUTUNER's own flash
   code is written, its eval harness diffs FUTUNER's outgoing UDS
   sequence against the MagicMotorsport reference. Bit-exact match is
   the bar.

**Tracked as Cowork task #6.** Capture-session checklist (exact
commands, file naming, on-paper data to record) to be drafted before
the actual session. Not yet drafted; not blocking other work.

---

## P-02 🔴 ECU simulator package built and validated against captures

Depends on P-01. Builds `firmware/test/ecu_sim/` — a Python ISO-TP
responder that loads recorded MagicMotorsport captures and replays the
ECU side. Lets FUTUNER's Phase 2 flash code iterate against a fake ECU
on the bench without touching the dev car. The simulator's correctness
is graded by replaying its responses against the original tester
queries from the same capture and verifying byte-exact match.

---

## P-03 🔴 Recovery binary per ECU family

Per MISSION_SPEC §5.1 and SCALE_ARCHITECTURE_PROPOSAL §7.4. A separate,
minimal binary per ECU family that the dongle attempts to apply if a
flash fails mid-write. Without this, a partial flash failure can brick
the ECU. Must exist before any production Phase 2 ever runs.

---

## P-04 🔴 Pre-flash safety gate hardened

Per SCALE_ARCHITECTURE_PROPOSAL §7.3. All gates implemented and tested:
variant ID matches binary, battery voltage above floor, ignition state
correct, ECU responding within timeout, customer license shows
`paid = true` and `phase2_flashed_at = null` (or admin override),
explicit customer UI confirmation within last N seconds. Configured
defaults: voltage floor 12.4 V, confirmation window 60 s — needs Sean's
sign-off.

---

## P-05 🔴 Per-variant base binary signed and stored

Per SCALE_ARCHITECTURE_PROPOSAL §7. SRM-built and SRM-signed Phase 2
base binary per ECU variant (Stage 1 power + logging + live-tune
hooks). Stored in object storage. Signed URLs for delivery. Firmware
verifies signature before flash.

---

## P-06 🔴 AES key custody decided and implemented

Per SCALE_ARCHITECTURE_PROPOSAL §10 question 3. Where do AES keys live
in production? HSM-backed cloud secret, encrypted-at-rest in cloud DB,
or split between cloud and dongle secrets partition? This is a
load-bearing decision for Phase 2 and an open question Sean has not
yet signed off on.

---

## P-07 🔴 Real-bench Phase 2 validation per variant

Each ECU variant in scope for Phase 2 must have at least one
real-bench-rig flash succeed end-to-end before any customer device
gets that variant's Phase 2 code. Simulated bench is not enough for
Phase 2 (fine for Phase 1).

---

## P-08 🔴 Dongle firmware Phase 2 flash code written + eval harness green

The actual flash module under `firmware/src/flash/` — Phase 2 specific
work. Lots of sub-tasks: UDS challenge-response mirroring, AES-128-CBC
implementation (or hardware-accelerated path on ESP32-S3), chunked
TransferData with per-block checksums, fault recovery sequence,
integration with feature_manager (`FEATURE_PHASE2_FLASH`), pre-flash
gate enforcement. Eval harness diffs FUTUNER output against the
MagicMotorsport reference from P-01 and asserts bit-exact match.

---

## P-09 🔴 Customer-facing Phase 2 documentation

Per MISSION_SPEC §5.1 deliverables. End-user-readable explanation of
what Phase 2 does, the safety considerations, and the "if anything
looks wrong" recovery procedure. Required before any customer
device is allowed to do a Phase 2 flash.

---

## P-10 🔴 Cloud server Phase 2 endpoints

Per SCALE_ARCHITECTURE_PROPOSAL §3.4. New endpoints:
- `GET /api/v1/phase2/base` — signed URL to this variant's base binary
- Admin endpoints for uploading + assigning per-variant binaries

These don't exist in `cloud/src/main.py` today.

---

## Update protocol

When you close an item out, change its emoji to 🟢 and add a one-line
note above the legend showing what changed and when. When you discover
a new prerequisite that's not on this list, add it as the next P-NN.
Do not silently re-order; the numbers are sticky.

When all P-items are 🟢, Phase 2 is unblocked from a prerequisites
standpoint. Even then: no Phase 2 firmware lands on a customer device
without Sean's explicit per-variant sign-off.
