#!/usr/bin/env python3
"""candump_to_shadow_log — convert a SocketCAN candump log (as produced
by python-can's can.Logger or `candump -L`) into the shadow-log format
that mdg1_transport_shadow.c writes and tools/flash_shadow_diff.py
consumes (`TX <hex>\n` / `RX <hex>\n` per UDS message).

Filters to CAN IDs 0x7E0 (tester→ECU, emitted as `TX`) and 0x7E8 (ECU→
tester, emitted as `RX`). All other IDs are silently dropped — this is
diagnostic UDS reassembly, not a general-purpose ISO-TP demultiplexer.

ISO-TP single-frames (PCI high-nibble = 0): the low nibble is the
payload length; the next N bytes are the UDS message. Emit one line
per frame.

ISO-TP multi-frame (FF/CF): a first-frame (high-nibble 1) advertises
a 12-bit total length and the first 6 payload bytes; subsequent
consecutive-frames (high-nibble 2) carry the remainder. We reassemble
per direction and emit one line when the message is complete. Flow-
control frames (high-nibble 3) are dropped — they don't carry payload
the diff tool cares about.

For the HIL preflight halt-before-erase window, only single-frame
exchanges should appear (SA seed/key + fingerprint write/ack). FF/CF
support is included so a multi-frame VIN read or any TransferData
chunk doesn't trip the adapter, but it isn't exercised by the
preflight workflow.

Usage:
    candump_to_shadow_log.py INPUT.log [OUTPUT.log]
    candump_to_shadow_log.py < INPUT.log > OUTPUT.log

Exit codes:
    0  parsed at least one TX or RX line
    1  no usable frames in input (likely wrong format or wrong IDs)
    2  argv / I/O error
"""

import re
import sys

# candump line shape (python-can's Logger default for .log):
#   (1747086523.148693) can0 7E0#023E000000000000 R
LINE_RE = re.compile(
    r"^\(\s*[\d.]+\s*\)\s+\S+\s+([0-9A-Fa-f]+)#([0-9A-Fa-f]*)(?:\s|$)"
)

TX_ID = 0x7E0
RX_ID = 0x7E8


def hex_to_bytes(s):
    if len(s) & 1:
        s = s[:-1]  # candump never emits odd-length but be defensive
    return bytes.fromhex(s)


def reassemble(frame_bytes):
    """Yield one or more UDS messages for the given run of ISO-TP frame
    bytes from a single CAN ID. State is held in the closure caller."""
    raise RuntimeError("not used — reassembly is done per-direction inline")


def process(in_stream, out_stream):
    # Per-direction reassembly state for FF/CF chains.
    state = {
        TX_ID: {"pending": None, "remaining": 0},
        RX_ID: {"pending": None, "remaining": 0},
    }
    n_out = 0
    for raw in in_stream:
        m = LINE_RE.match(raw)
        if not m:
            continue
        try:
            cid = int(m.group(1), 16)
        except ValueError:
            continue
        if cid not in (TX_ID, RX_ID):
            continue
        try:
            data = hex_to_bytes(m.group(2))
        except ValueError:
            continue
        if not data:
            continue

        direction = "TX" if cid == TX_ID else "RX"
        st = state[cid]
        pci_hi = (data[0] >> 4) & 0x0F

        if pci_hi == 0x0:
            # Single-frame: low nibble = length, payload = data[1:1+len]
            length = data[0] & 0x0F
            if length == 0 or length > 7:
                continue
            payload = data[1:1 + length]
            if len(payload) != length:
                continue
            out_stream.write(f"{direction} {payload.hex()}\n")
            n_out += 1
            # Any in-flight multi-frame for this direction is abandoned
            # by a SF (rare, but defensive).
            st["pending"] = None
            st["remaining"] = 0

        elif pci_hi == 0x1:
            # First-frame: 12-bit length spans low nibble of byte 0 + byte 1.
            if len(data) < 8:
                continue
            total_len = ((data[0] & 0x0F) << 8) | data[1]
            if total_len < 8:
                continue  # FF shouldn't be used for <8-byte messages
            st["pending"] = bytearray(data[2:8])  # first 6 payload bytes
            st["remaining"] = total_len - 6

        elif pci_hi == 0x2:
            # Consecutive-frame: low nibble = sequence number (ignored
            # for diff-tool purposes — the adapter doesn't validate
            # sequencing; that's the orchestrator's job).
            if st["pending"] is None or st["remaining"] <= 0:
                continue
            take = min(st["remaining"], len(data) - 1)
            st["pending"].extend(data[1:1 + take])
            st["remaining"] -= take
            if st["remaining"] <= 0:
                payload = bytes(st["pending"])
                out_stream.write(f"{direction} {payload.hex()}\n")
                n_out += 1
                st["pending"] = None
                st["remaining"] = 0

        elif pci_hi == 0x3:
            # Flow-control: skip. Diff tool doesn't compare FC bytes.
            continue
        else:
            # Reserved PCI types — skip.
            continue

    return n_out


def main(argv):
    if len(argv) >= 2:
        try:
            in_stream = open(argv[1], "r", encoding="utf-8", errors="replace")
        except OSError as e:
            print(f"candump_to_shadow_log: cannot open {argv[1]}: {e}",
                  file=sys.stderr)
            return 2
    else:
        in_stream = sys.stdin

    if len(argv) >= 3:
        try:
            out_stream = open(argv[2], "w", encoding="utf-8")
        except OSError as e:
            print(f"candump_to_shadow_log: cannot write {argv[2]}: {e}",
                  file=sys.stderr)
            in_stream.close()
            return 2
    else:
        out_stream = sys.stdout

    try:
        n = process(in_stream, out_stream)
    finally:
        if in_stream is not sys.stdin:
            in_stream.close()
        if out_stream is not sys.stdout:
            out_stream.close()

    if n == 0:
        print("candump_to_shadow_log: no 0x7E0/0x7E8 frames produced "
              "a UDS line — check input format and CAN IDs",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
