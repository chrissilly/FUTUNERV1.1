# Bench CAN Toolkit — spec

> Status: spec for tandem Claude Code session. Implementation lives under
> `firmware/test/can_capture/` and is built by that session, not the
> firmware sessions.

---

## What this is

A host-side toolkit Claude Code uses to monitor the dongle's CAN bus
behavior during automated test iterations. The dongle's own serial log
tells you what the firmware **thinks** it did. The CAN bus, captured
via a Candlelight USB-CAN sniffer, tells you what **actually** went on
the wire. When the two disagree, the wire wins.

Without this toolkit, every UDS bug becomes guesswork. With it, the
agent gets a third independent witness (firmware log + cloud telemetry
+ wire capture) before it can declare a fix shipped.

---

## Hardware setup

### The sniffer

[Candlelight USB-CAN](https://github.com/HubertD/candleLight) — a
$30-$50 USB-to-CAN bridge built on STM32 + the open-source
`candlelight_fw` (gs_usb) firmware. On Linux it shows up as a SocketCAN
device (`can0`) natively. On macOS, `python-can` talks to it via
libusb using the `gs_usb` backend.

Linux is dramatically easier for this work because the SocketCAN
toolchain (`candump`, `cansniffer`, `isotpsend`, `isotprecv`, the
kernel `isotp` module) is built for exactly this job. macOS works but
needs more Python glue.

### Wiring topology — IMPORTANT

The dongle and the Candlelight have to share the diagnostic CAN bus.
There's only one OBD-II port. Two valid ways:

1. **OBD-II Y-splitter (recommended).** Passive pass-through that gives
   you two OBD-II ports off one. Both the dongle and the Candlelight
   plug in. They see the same frames. Cost: ~$30 generic, ~$60 for an
   isolated-ground variant. **Use the isolated variant** when you can —
   it keeps the Candlelight's USB ground from coupling to the car
   ground, which is a real noise source on long sessions.
2. **Splice the dongle harness.** Tap CAN H/L from the dongle's CAN
   transceiver pins directly. Avoids the splitter cost. **Only ever do
   this on a dev rig.** Never on a customer dongle.

A `--target=bench` lockout in the toolkit (see Safety, below) refuses
to send anything to the bus when the bus might be a customer car. Don't
disable it.

### Bus speed

500 kbps. Always. Set it explicitly on the SocketCAN interface; never
let it auto-negotiate. The dongle is hardcoded to 500 kbps per
`BOARD_REV2`.

---

## Toolkit API

All scripts live under `firmware/test/can_capture/`. Each has a single
responsibility, takes flags, exits with a clear status code.

### Setup and teardown

`bench/can_setup.sh [--iface=can0] [--bitrate=500000]`
- Brings the SocketCAN interface up at the configured bitrate.
- Refuses to run if the interface is already up in a different state
  (mismatch is almost always a configuration error you want to know
  about, not silently override).
- Idempotent if the interface is already up at the right bitrate.

`bench/can_teardown.sh [--iface=can0]`
- Brings the interface down cleanly. Run between captures so a stale
  interface doesn't carry state into the next iteration.

### Capture

`bench/capture_start.sh [--iface=can0] [--name=feature_under_test] [--max-seconds=120]`
- Starts a `candump -tz -L` capture in the background.
- Names the log file `captures/<timestamp>_<name>.candump`.
- Records the capture PID to a lockfile so capture_stop can find it.
- Refuses to run a second capture while one is active (one capture at
  a time per iteration).

`bench/capture_stop.sh`
- Stops the active capture cleanly via the lockfile PID.
- Prints the path to the produced log file.

### Parse

`bench/parse_uds.py <capture.candump> [--out=parsed.jsonl] [--variant=<ecu_variant_id>]`
- Reads a candump log.
- Reassembles ISO-TP messages from raw frames (using the kernel `isotp`
  module on Linux when available, falling back to `python-can-isotp`
  otherwise).
- Decodes UDS services to human-readable form. Each output line is one
  JSON object on stdout (or to `--out`):
  ```json
  {
    "ts": 1714892100.123456,
    "dir": "tx",
    "src": "0x7E0",
    "dst": "0x710",
    "service": "ReadDataByIdentifier",
    "service_id": "0x22",
    "did": "0xF190",
    "raw": "22 F1 90",
    "decoded": null
  }
  ```
  Responses include `"decoded": "WAUZZZ4M..."` where the toolkit knows
  the DID's semantics; otherwise `decoded` is null.
- Per-variant decoding (e.g., the right ethanol-address interpretation
  for `4K0907557G__0003`) reads from the variant manifest at
  `docs/boxcode_database.json`.

### Send (read-only by default)

`bench/cansend_safe.sh <args...>`
- Wraps `cansend` with a hard refusal unless **both** flags are passed:
  `--allow-tx --target=bench`. The flags must be the first two arguments;
  positional, not buried in `args...`.
- Without those flags, exits 2 with a clear message.
- This is the single point where any frame can be transmitted. Do not
  call `cansend` directly anywhere else in the toolkit.
- Logs every transmitted frame to `tx_log/<timestamp>.tx.log` for audit.

### Eval

`bench/eval.sh`
- The graded harness. Exit 0 on all-pass, non-zero on any failure.
- Sections: file structure, safety guard rejects tx without flags,
  parser produces expected output for synthetic fixtures, no
  modifications to forbidden directories, README + this spec doc
  reference each other consistently, Python deps importable.

---

## Synthetic fixtures

`firmware/test/can_capture/fixtures/` holds:

- `<name>.candump` — a hand-crafted candump log file (real format, but
  synthesized, not from a live car).
- `<name>.expected.jsonl` — the parser's expected output for that
  capture.
- `<name>.notes.md` — what UDS sequence the fixture represents and
  why.

The toolkit's parser must produce output that exactly matches the
expected file (modulo timestamps, which are normalized by the eval
harness). One fixture is provided as a starting contract:
`fixtures/read_vin.*` — a tester→ECU `0x22 F1 90` ReadDataByIdentifier
followed by an ECU multi-frame ISO-TP response containing a VIN string.

The tandem session may add more fixtures. The contract is: every
fixture has the three-file pair, and the parser passes all of them.

---

## Safety guarantees

These are the invariants the toolkit must enforce. The eval harness
verifies each one.

1. **No transmission without explicit opt-in.** The parser, the capture
   scripts, and the setup scripts are all read-only. Only `cansend_safe.sh`
   can transmit, and only with `--allow-tx --target=bench` as the first
   two args. Default mode is "snoop the bus, never write."

2. **Bench-target enforcement.** The `--target=bench` flag is required
   for any tx. There is no `--target=car` option. If a future use case
   needs car-targeted tx, that's a separate, reviewed change — it does
   not get added by an autonomous Claude Code session.

3. **No firmware modifications.** The toolkit lives entirely in
   `firmware/test/can_capture/`. It does not touch `firmware/src/`,
   the frozen modules, the cloud server, or any other part of the
   project. The eval harness checks this with git status.

4. **No interference with the dongle's own CAN traffic.** The
   Candlelight is plumbed in passively (Y-splitter or read-only tap);
   the toolkit reads, parses, and decodes — it does not inject. The
   dongle owns the bus when it's running.

---

## How Claude Code uses this toolkit

Per-iteration flow during firmware development:

```
1. Build firmware on dev machine.
2. Flash to bench dongle (or push via cloud OTA for car-connected dongle).
3. bench/can_setup.sh
4. bench/capture_start.sh --name=wot_logger_test
5. Send the test command (websocket or serial) to the dongle.
6. Wait the expected duration.
7. bench/capture_stop.sh
8. bench/parse_uds.py captures/<latest>.candump --out=parsed.jsonl
9. Run feature-specific eval against parsed.jsonl, the firmware serial
   log, and cloud telemetry events.
10. Pass or fail; loop.
```

The CAN log is the test oracle for anything that interacts with the
ECU. The firmware serial log is the oracle for state transitions.
Cloud telemetry is the oracle for end-to-end behavior. All three must
agree before a fix ships.

---

## What this toolkit deliberately does NOT do

- It does not flash firmware. That stays in `firmware/test/bench/`
  (separate concern, separate session, future work).
- It does not orchestrate end-to-end iterations. Each script does one
  thing; the orchestration is per-feature and lives with the feature's
  own eval harness (e.g., `firmware/test/wot_logger/eval.sh` will call
  this toolkit).
- It does not know about specific features. The parser knows about
  UDS at the protocol layer; everything semantic
  ("did the WOT logger fire correctly?") is a feature-level concern.
- It does not transmit on a customer car bus. Ever. Even with all the
  flags in the world, the toolkit refuses unless `--target=bench`.
