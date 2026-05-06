# Bench CAN Toolkit

Host-side scripts and a UDS parser used by Claude Code (and humans) to
monitor the dongle's CAN bus during automated test iterations.

> The canonical spec for this toolkit is
> [`docs/BENCH_CAN_TOOLKIT.md`](../../../docs/BENCH_CAN_TOOLKIT.md).
> Read that first. This README is a quickstart.

---

## What this toolkit is for

Three independent witnesses must agree before a fix ships: the firmware
serial log, the cloud telemetry, and the wire capture. This toolkit is
the wire capture half — bring up the SocketCAN interface, capture
traffic during a test, and decode the ISO-TP / UDS exchange to JSONL
that downstream feature-level evals can assert against.

The toolkit is **read-only by default**. The only script that can
transmit is `bench/cansend_safe.sh`, and only with the exact opt-in
flags `--allow-tx --target=bench` as the first two arguments. There is
no `--target=car` option.

---

## Prerequisites

### Linux (primary target)

- Kernel SocketCAN (`can`, `can_raw`, `can_isotp` modules — most modern
  distros include them).
- `can-utils` package (`candump`, `cansend`, `ip` link helpers).
- Python 3.10+ recommended; 3.9 is the minimum the parser is tested
  against. (The parser uses `from __future__ import annotations` so
  modern type-hint syntax works on 3.9.)
- Root or `sudo` rights to bring `can0` up/down.

### macOS (best effort)

- libusb installed (`brew install libusb`).
- python-can with the `gs_usb` backend, talking to the Candlelight via
  USB. There is no native SocketCAN on macOS, so the SocketCAN-only
  scripts (`can_setup.sh`, `can_teardown.sh`, the candump-based
  capture pair, `cansend_safe.sh`) exit with a clear "Linux only"
  message. The parser (`parse_uds.py`) and the eval harness work
  fine — they read `.candump` log files, which can be produced on a
  Linux host or fed in as fixtures.

### Python deps

```bash
pip install python-can==4.6.1 can-isotp==2.0.6 pytest==8.4.2
```

(Or `pip install --user ...` if you don't have a venv set up.)
See [`requirements.txt`](requirements.txt) for why the file lists
import names rather than PyPI package names.

---

## Layout

```
firmware/test/can_capture/
├── README.md                       ← you are here
├── requirements.txt                ← Python deps (importable names)
├── eval.sh                         ← graded harness (do not modify)
├── bench/
│   ├── defaults.cfg                ← project-wide tunables (sourced by every script)
│   ├── can_setup.sh                ← bring can0 up at 500 kbps
│   ├── can_teardown.sh             ← bring can0 down cleanly
│   ├── capture_start.sh            ← background candump → captures/<ts>_<name>.candump
│   ├── capture_stop.sh             ← stop capture via lockfile, print log path
│   ├── cansend_safe.sh             ← the ONLY tx path; refuses tx without --allow-tx --target=bench
│   └── parse_uds.py                ← candump → JSONL UDS decoder (importable + CLI)
├── tests/
│   └── test_parse_uds.py           ← pytest suite for the parser internals
└── fixtures/
    ├── read_vin.{candump,expected.jsonl,notes.md}            ← starter (multi-frame ISO-TP)
    ├── negative_response.{candump,expected.jsonl,notes.md}   ← UDS 0x7F NRC handling
    └── multi_did_read.{candump,expected.jsonl,notes.md}      ← ReadDataByIdentifier with 2 DIDs
```

Captures land in `captures/` (gitignored), tx audit logs in `tx_log/`
(gitignored). Both directories are created on first use by their
respective scripts.

---

## Quickstart — sample capture-and-parse cycle (Linux)

```bash
# 1. Bring the bus up.
firmware/test/can_capture/bench/can_setup.sh                # uses defaults.cfg

# 2. Start capturing in the background.
firmware/test/can_capture/bench/capture_start.sh --name=read_vin_demo

# 3. Trigger your test (websocket command, serial command, button push).
#    The dongle's UDS traffic is now hitting the bus.

# 4. Stop the capture.
firmware/test/can_capture/bench/capture_stop.sh
# → prints e.g. captures/20260505T120000_read_vin_demo.candump

# 5. Decode to JSONL.
firmware/test/can_capture/bench/parse_uds.py \
    captures/20260505T120000_read_vin_demo.candump \
    --out=parsed.jsonl

# 6. Inspect.
head parsed.jsonl
```

To bring the interface back down between iterations:

```bash
firmware/test/can_capture/bench/can_teardown.sh
```

---

## Self-check

```bash
cd ~/esp/obd/FUTV1.1
firmware/test/can_capture/eval.sh
```

Exits 0 if the toolkit is healthy. The eval grades against synthetic
fixtures only — no real hardware needed.

```bash
pytest firmware/test/can_capture/tests/
```

Runs the parser's unit tests in isolation from the eval gate.

---

## Conventions

- Every shell script reads `bench/defaults.cfg` for tunables. No
  bitrates, interface names, or address pairs are hardcoded inline
  (project's "no magic numbers" rule).
- All scripts exit 0 on success and document non-zero exit codes
  near the top of the file. Errors go to stderr; data goes to stdout.
- The parser's JSONL schema is defined in `docs/BENCH_CAN_TOOLKIT.md`
  §"Toolkit API → Parse". Each line is one UDS message (not one CAN
  frame); ISO-TP reassembly happens transparently.

---

## What this toolkit is NOT

- It does **not** flash firmware (separate concern, separate session).
- It does **not** orchestrate end-to-end test loops (that lives with
  each feature's own eval, e.g. `firmware/test/wot_logger/eval.sh`).
- It does **not** know feature semantics — only ISO-TP reassembly and
  the UDS service layer. Feature-level decoding (e.g. "did the WOT
  logger fire correctly?") is the caller's job.
- It does **not** transmit on a customer car bus, ever, regardless of
  flags. `cansend_safe.sh` only honors `--target=bench`.
