#!/usr/bin/env python3
"""
extract_mm_expected_responses.py

Parse a MagicMotorsport CAN-log capture (`mm_FULL_Flash.log` or similar
candump) and emit a JSON fixture that the shadow transport plays back
as ECU responses.

Output schema:
    {
      "boxcode": "4K0907557G__0003",
      "source_log": "<absolute path of input log>",
      "extracted_at_utc": "...",
      "responses": [
        {"tx_hex": "1003",          "rx_hex": "50031E01E0"},
        {"tx_hex": "22F190",        "rx_hex": "62F190..."},
        ...
      ]
    }

The TX/RX pairs are ordered: each `tx_hex` is a UDS request message
(SID first, no PCI bytes), each `rx_hex` is the corresponding UDS
response. The shadow transport prefix-matches against `tx_hex` and
replays the matching `rx_hex` in order.

Usage:
    extract_mm_expected_responses.py \
        --input  /Users/rabbit/sniffer/mm_FULL_Flash.log \
        --output firmware/test/can_capture/fixtures/expected_responses_4K0907557G_0003.json \
        --boxcode 4K0907557G__0003

stdlib only.
"""
import argparse
import datetime
import json
import os
import sys
from pathlib import Path

REQUEST_ID = "7E0"
RESPONSE_ID = "7E8"


def iter_candump_frames(path):
    """Yield (can_id, payload_bytes) for every CAN data line in path."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for ln in f:
            if "#" not in ln:
                continue
            try:
                left, right = ln.split("#", 1)
                payload_hex = right.split()[0]
                parts = left.rsplit(None, 1)
                if len(parts) != 2:
                    continue
                _, can_id = parts
                payload = bytes.fromhex(payload_hex)
            except (ValueError, IndexError):
                continue
            yield can_id, payload


def reassemble_isotp(path, only_id):
    """Walk a candump and yield reassembled UDS messages for one CAN ID,
    in chronological order. ISO-TP single-frame / first-frame /
    consecutive-frame handling. Flow control frames are skipped."""
    state = None
    for can_id, data in iter_candump_frames(path):
        if can_id != only_id or not data:
            continue
        b0 = data[0]
        high = b0 >> 4
        if high == 0:
            # Single frame
            length = b0 & 0xF
            msg = bytes(data[1:1 + length])
            state = None
            if msg:
                yield msg
        elif high == 1:
            # First frame
            length = ((b0 & 0xF) << 8) | data[1]
            state = {"len": length, "buf": bytearray(data[2:8])}
        elif high == 2:
            if state is None:
                continue
            state["buf"].extend(data[1:8])
            if len(state["buf"]) >= state["len"]:
                msg = bytes(state["buf"][:state["len"]])
                state = None
                if msg:
                    yield msg
        # high == 3 → flow control (skip)


def pair_requests_responses(requests, responses):
    """Walk two same-length-ish iterators and produce ordered (tx, rx) pairs.

    The MM capture interleaves frames in time; on the bus, every TX is
    promptly followed by one or more RX (positive + possibly
    pending-then-final). We approximate by zipping the two streams in
    order and skipping pending negative responses (7F xx 78)."""
    req_iter = iter(requests)
    rsp_iter = iter(responses)
    rsp_buffer = []
    while True:
        try:
            req = next(req_iter)
        except StopIteration:
            return

        # Consume any leftover responses from previous pending sequence.
        if rsp_buffer:
            rx = rsp_buffer.pop(0)
        else:
            try:
                rx = next(rsp_iter)
            except StopIteration:
                return

        # Skip pending negative responses (7F xx 78) — they don't pair
        # with a new request, they pair with the same request as the
        # final response. So treat them as transparent and pull again.
        while len(rx) >= 3 and rx[0] == 0x7F and rx[2] == 0x78:
            try:
                rx = next(rsp_iter)
            except StopIteration:
                return

        yield req, rx


def main():
    ap = argparse.ArgumentParser(description="Extract UDS TX/RX pairs from an MM CAN log.")
    ap.add_argument("--input", required=True, help="Path to mm_*.log (candump format).")
    ap.add_argument("--output", required=True, help="Output JSON fixture path.")
    ap.add_argument("--boxcode", required=True, help="Boxcode this capture is for (e.g. 4K0907557G__0003).")
    args = ap.parse_args()

    if not Path(args.input).is_file():
        sys.exit(f"input not found: {args.input}")

    requests = list(reassemble_isotp(args.input, REQUEST_ID))
    responses = list(reassemble_isotp(args.input, RESPONSE_ID))

    pairs = []
    for req, rx in pair_requests_responses(requests, responses):
        pairs.append({
            "tx_hex": req.hex().lower(),
            "rx_hex": rx.hex().lower(),
        })

    out = {
        "boxcode": args.boxcode,
        "source_log": os.path.abspath(args.input),
        "extracted_at_utc": datetime.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "responses": pairs,
    }
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
        f.write("\n")
    print(f"wrote {len(pairs)} TX/RX pairs to {args.output}")


if __name__ == "__main__":
    main()
