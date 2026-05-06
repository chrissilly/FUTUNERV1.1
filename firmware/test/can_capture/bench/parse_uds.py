#!/usr/bin/env python3
"""parse_uds.py — candump (-tz -L) → JSONL UDS message decoder.

Reads a candump log file produced by `candump -tz -L`, reassembles
ISO-TP segmented messages into whole UDS payloads, and emits one JSON
object per UDS message in the format documented in
docs/BENCH_CAN_TOOLKIT.md §"Toolkit API → Parse".

This module is both an importable library (the IsoTpReassembler and
decode_uds functions are stable) and a CLI. The CLI is used by the
eval harness; the library is used by the pytest suite under
firmware/test/can_capture/tests/.

ISO-TP reassembly is implemented in pure Python — we don't depend on
the kernel isotp module or python-can-isotp here, because the parser
is operating offline on a candump log file (no live socket). The
runtime detection of those backends in the spec is for the live
SocketCAN path, which this parser does not use.

Per FUTV1.1/CLAUDE.md hard rule 3 (no magic numbers): the
tester/ECU CAN ID pair, default interface, and other tunables are
read from bench/defaults.cfg. CLI flags override.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Iterable, Iterator, Optional


# ---------------------------------------------------------------------------
# Defaults (loaded from bench/defaults.cfg at module import time).
# ---------------------------------------------------------------------------

_DEFAULTS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "defaults.cfg")


def _load_defaults(path: str) -> dict[str, str]:
    """Parse a KEY=VALUE config file. Comments (#) and blanks ignored."""
    out: dict[str, str] = {}
    if not os.path.exists(path):
        return out
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


_DEFAULTS = _load_defaults(_DEFAULTS_PATH)
DEFAULT_TESTER_ID = int(_DEFAULTS.get("TESTER_REQUEST_ID", "0x7E0"), 16)
DEFAULT_ECU_ID = int(_DEFAULTS.get("ECU_RESPONSE_ID", "0x710"), 16)


# ---------------------------------------------------------------------------
# UDS service / NRC tables (per ISO 14229-1, scoped to what the dongle uses
# — see docs/CAN_UDS_PROTOCOL.md). Adding a service here is fine; the
# parser will pick it up automatically.
# ---------------------------------------------------------------------------

UDS_SERVICES: dict[int, str] = {
    0x10: "DiagnosticSessionControl",
    0x11: "ECUReset",
    0x14: "ClearDiagnosticInformation",
    0x19: "ReadDTCInformation",
    0x22: "ReadDataByIdentifier",
    0x23: "ReadMemoryByAddress",
    0x27: "SecurityAccess",
    0x28: "CommunicationControl",
    0x2E: "WriteDataByIdentifier",
    0x2F: "InputOutputControlByIdentifier",
    0x31: "RoutineControl",
    0x34: "RequestDownload",
    0x35: "RequestUpload",
    0x36: "TransferData",
    0x37: "RequestTransferExit",
    0x3D: "WriteMemoryByAddress",
    0x3E: "TesterPresent",
}

UDS_NRCS: dict[int, str] = {
    0x10: "General Reject",
    0x11: "Service Not Supported",
    0x12: "Sub-Function Not Supported",
    0x13: "Incorrect Message Length Or Invalid Format",
    0x22: "Conditions Not Correct",
    0x31: "Request Out Of Range",
    0x33: "Security Access Denied",
    0x35: "Invalid Key",
    0x36: "Exceed Number Of Attempts",
    0x37: "Required Time Delay Not Expired",
    0x70: "Upload Download Not Accepted",
    0x71: "Transfer Data Suspended",
    0x72: "General Programming Failure",
    0x78: "Response Pending",
    0x7E: "Sub-Function Not Supported In Active Session",
    0x7F: "Service Not Supported In Active Session",
}

# DIDs that carry ASCII string values (per ISO 15031-6 / dongle's usage).
# Values are (description, fixed_length_or_None).
ASCII_DIDS: dict[int, tuple[str, Optional[int]]] = {
    0xF190: ("VIN", 17),                # ISO 15031-6: always 17 ASCII chars
    0xF187: ("VehicleManufacturerSparePartNumber", None),
    0xF189: ("VehicleManufacturerECUSoftwareVersionNumber", None),
    0xF1F4: ("BuildID", None),
}


# ---------------------------------------------------------------------------
# candump line parsing
# ---------------------------------------------------------------------------

# `candump -tz -L` line format:
#   (1714892100.000123) can0 7E0#0322F19000000000
# Leading zeros on the integer-seconds component are allowed (some
# distros emit them; we strip them with float()).
_CANDUMP_RE = re.compile(
    r"\((?P<ts>\d+(?:\.\d+)?)\)\s+(?P<iface>\S+)\s+(?P<id>[0-9A-Fa-f]+)#(?P<data>[0-9A-Fa-f]*)"
)


@dataclass
class CanFrame:
    ts: float
    iface: str
    can_id: int
    data: bytes


def parse_candump_line(line: str) -> Optional[CanFrame]:
    """Parse one candump log line. Returns None for blank/comment lines."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    m = _CANDUMP_RE.match(line)
    if not m:
        return None
    data_hex = m.group("data")
    if len(data_hex) % 2 != 0:
        # Malformed CAN frame (odd nibbles). Skip it; the caller can
        # decide whether to log; we fail closed and don't guess at
        # padding because it would silently corrupt reassembly.
        return None
    return CanFrame(
        ts=float(m.group("ts")),
        iface=m.group("iface"),
        can_id=int(m.group("id"), 16),
        data=bytes.fromhex(data_hex),
    )


# ---------------------------------------------------------------------------
# ISO-TP reassembly
# ---------------------------------------------------------------------------

@dataclass
class _IsoTpBuffer:
    first_ts: float
    length: int
    payload: bytearray = field(default_factory=bytearray)
    next_seq: int = 1  # CF sequence numbers start at 1, wrap 0–F


@dataclass
class UdsMessage:
    """A reassembled UDS message ready for service-layer decoding."""
    ts: float
    src_id: int
    payload: bytes


class IsoTpReassembler:
    """Stateful per-CAN-ID ISO-TP reassembler.

    Tracks a separate buffer per source CAN ID so requests and responses
    on a request/response pair don't interfere. Returns a UdsMessage
    when a message completes; returns None for in-progress fragments
    and for flow-control frames (which carry no UDS payload).
    """

    def __init__(self) -> None:
        self._buffers: dict[int, _IsoTpBuffer] = {}

    def feed(self, frame: CanFrame) -> Optional[UdsMessage]:
        if not frame.data:
            return None
        pci = frame.data[0]
        pci_type = pci >> 4

        if pci_type == 0x0:
            # Single Frame: low nibble = payload length.
            length = pci & 0x0F
            if length == 0 or length > min(7, len(frame.data) - 1):
                return None
            payload = bytes(frame.data[1 : 1 + length])
            return UdsMessage(ts=frame.ts, src_id=frame.can_id, payload=payload)

        if pci_type == 0x1:
            # First Frame: 12-bit total length across PCI byte 0 low
            # nibble + byte 1; then 6 bytes of payload.
            if len(frame.data) < 8:
                return None
            length = ((pci & 0x0F) << 8) | frame.data[1]
            if length <= 7:
                # FF declares ≤7 bytes — should have been a SF; skip.
                return None
            self._buffers[frame.can_id] = _IsoTpBuffer(
                first_ts=frame.ts,
                length=length,
                payload=bytearray(frame.data[2:8]),
                next_seq=1,
            )
            return None

        if pci_type == 0x2:
            # Consecutive Frame: low nibble = sequence number (1..F, wraps).
            buf = self._buffers.get(frame.can_id)
            if buf is None:
                return None  # stray CF, ignore
            seq = pci & 0x0F
            expected = buf.next_seq & 0x0F
            if seq != expected:
                # Sequence break → drop the in-progress reassembly.
                # A real implementation would also alert; we silently
                # discard so a flaky bus doesn't kill a whole capture.
                del self._buffers[frame.can_id]
                return None
            remaining = buf.length - len(buf.payload)
            take = min(7, remaining, len(frame.data) - 1)
            buf.payload.extend(frame.data[1 : 1 + take])
            buf.next_seq += 1
            if len(buf.payload) >= buf.length:
                msg = UdsMessage(
                    ts=buf.first_ts,
                    src_id=frame.can_id,
                    payload=bytes(buf.payload[: buf.length]),
                )
                del self._buffers[frame.can_id]
                return msg
            return None

        if pci_type == 0x3:
            # Flow Control: no UDS payload, do not emit.
            return None

        # Unknown PCI; ignore.
        return None


# ---------------------------------------------------------------------------
# UDS service decoding
# ---------------------------------------------------------------------------

def _hex_id(value: int) -> str:
    """Format a CAN ID as the spec's hex string (e.g. 0x7E0)."""
    return f"0x{value:X}"


def _hex_byte(value: int) -> str:
    return f"0x{value:02X}"


def _hex_did(value: int) -> str:
    return f"0x{value:04X}"


def _raw_bytes(payload: bytes) -> str:
    return " ".join(f"{b:02X}" for b in payload)


def _decode_did_value(did: int, data: bytes) -> Optional[str]:
    """Best-effort ASCII decode for known string-valued DIDs."""
    if did not in ASCII_DIDS:
        return None
    _, fixed_len = ASCII_DIDS[did]
    chunk = data[:fixed_len] if fixed_len is not None else data
    chunk = chunk.rstrip(b"\x00")
    if not chunk:
        return None
    try:
        text = chunk.decode("ascii")
    except UnicodeDecodeError:
        return None
    # Reject decoded values with control characters — usually a sign
    # the payload wasn't actually ASCII (e.g. multi-DID response where
    # the data length is unknown and we sliced through binary).
    if any(ord(c) < 0x20 or ord(c) > 0x7E for c in text):
        return None
    return text


def decode_uds(
    msg: UdsMessage,
    tester_id: int = DEFAULT_TESTER_ID,
    ecu_id: int = DEFAULT_ECU_ID,
) -> Optional[dict]:
    """Turn one reassembled UDS message into the JSON record dict.

    Returns None for messages that aren't from the configured pair
    (we don't speculate about the direction of off-pair traffic).
    """
    payload = msg.payload
    if not payload:
        return None

    if msg.src_id == tester_id:
        direction = "tx"
        src, dst = tester_id, ecu_id
    elif msg.src_id == ecu_id:
        direction = "rx"
        src, dst = ecu_id, tester_id
    else:
        return None

    sid = payload[0]

    # Negative response: 0x7F <rejected_sid> <nrc>
    if sid == 0x7F:
        rejected_sid = payload[1] if len(payload) > 1 else 0
        nrc = payload[2] if len(payload) > 2 else 0
        rejected_name = UDS_SERVICES.get(rejected_sid, f"0x{rejected_sid:02X}")
        nrc_name = UDS_NRCS.get(nrc, "Unknown NRC")
        return {
            "ts": msg.ts,
            "dir": direction,
            "src": _hex_id(src),
            "dst": _hex_id(dst),
            "service": "Negative Response",
            "service_id": _hex_byte(sid),
            "did": None,
            "raw": _raw_bytes(payload),
            "decoded": (
                f"rejected={rejected_name} ({_hex_byte(rejected_sid)}), "
                f"nrc={_hex_byte(nrc)} ({nrc_name})"
            ),
        }

    is_response = (sid & 0x40) != 0
    base_sid = sid - 0x40 if is_response else sid
    service_name = UDS_SERVICES.get(base_sid, f"Unknown service ({_hex_byte(base_sid)})")
    if is_response:
        service_name = f"{service_name} (positive response)"

    record: dict = {
        "ts": msg.ts,
        "dir": direction,
        "src": _hex_id(src),
        "dst": _hex_id(dst),
        "service": service_name,
        "service_id": _hex_byte(sid),
        "did": None,
        "raw": _raw_bytes(payload),
        "decoded": None,
    }

    # Service-specific decoding
    if base_sid == 0x22:  # ReadDataByIdentifier
        _decode_read_data_by_id(record, payload, is_response)

    return record


def _decode_read_data_by_id(record: dict, payload: bytes, is_response: bool) -> None:
    """Fill record['did'] and record['decoded'] for a ReadDataByIdentifier."""
    if not is_response:
        # Request: 22 [DID_hi DID_lo]+
        did_bytes = payload[1:]
        dids: list[int] = []
        for i in range(0, len(did_bytes) - 1, 2):
            dids.append((did_bytes[i] << 8) | did_bytes[i + 1])
        if dids:
            record["did"] = ",".join(_hex_did(d) for d in dids)
        return

    # Positive response: 62 [DID_hi DID_lo data*]+
    # Multi-DID responses don't carry per-DID lengths on the wire (ISO
    # 14229), so we can only safely decode multi-DID by walking the
    # known-DID table and using its fixed length when set. Any DID
    # without a known fixed length consumes the rest of the payload.
    body = payload[1:]
    parsed_dids: list[int] = []
    decoded_parts: list[str] = []
    pos = 0
    while pos + 2 <= len(body):
        did = (body[pos] << 8) | body[pos + 1]
        pos += 2
        # Determine how many data bytes belong to this DID.
        if did in ASCII_DIDS:
            _, fixed_len = ASCII_DIDS[did]
            if fixed_len is None:
                # No known length — the rest belongs to this DID.
                data = body[pos:]
                pos = len(body)
            else:
                data = body[pos : pos + fixed_len]
                pos += fixed_len
        else:
            # Unknown DID — give it everything remaining (single-DID
            # case is the only safe one without a length table).
            data = body[pos:]
            pos = len(body)

        parsed_dids.append(did)
        text = _decode_did_value(did, data)
        if text is not None:
            decoded_parts.append(f"{_hex_did(did)}={text}" if len(parsed_dids) != 1 or len(body) > 2 + len(data) else text)

    if parsed_dids:
        record["did"] = ",".join(_hex_did(d) for d in parsed_dids)

    if not decoded_parts:
        return
    if len(parsed_dids) == 1 and len(decoded_parts) == 1 and "=" not in decoded_parts[0]:
        record["decoded"] = decoded_parts[0]
    else:
        record["decoded"] = ", ".join(decoded_parts)


# ---------------------------------------------------------------------------
# Top-level pipeline
# ---------------------------------------------------------------------------

def parse_capture(
    lines: Iterable[str],
    tester_id: int = DEFAULT_TESTER_ID,
    ecu_id: int = DEFAULT_ECU_ID,
) -> Iterator[dict]:
    """Yield JSON record dicts for every complete UDS message in `lines`."""
    reassembler = IsoTpReassembler()
    for line in lines:
        frame = parse_candump_line(line)
        if frame is None:
            continue
        msg = reassembler.feed(frame)
        if msg is None:
            continue
        record = decode_uds(msg, tester_id=tester_id, ecu_id=ecu_id)
        if record is None:
            continue
        yield record


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("capture", help="Path to a candump -tz -L log file (use '-' for stdin).")
    p.add_argument("--out", help="Write JSONL here instead of stdout.")
    p.add_argument(
        "--tester-id",
        type=lambda s: int(s, 0),
        default=DEFAULT_TESTER_ID,
        help="Tester (request) CAN ID. Default from defaults.cfg.",
    )
    p.add_argument(
        "--ecu-id",
        type=lambda s: int(s, 0),
        default=DEFAULT_ECU_ID,
        help="ECU (response) CAN ID. Default from defaults.cfg.",
    )
    p.add_argument(
        "--variant",
        help="ECU variant ID for per-variant decoding (placeholder for boxcode_database lookup).",
    )
    args = p.parse_args(argv)

    if args.capture == "-":
        lines: Iterable[str] = sys.stdin
        sink = sys.stdout if not args.out else open(args.out, "w")
    else:
        lines = open(args.capture)
        sink = sys.stdout if not args.out else open(args.out, "w")

    try:
        for record in parse_capture(lines, tester_id=args.tester_id, ecu_id=args.ecu_id):
            sink.write(json.dumps(record) + "\n")
        sink.flush()
    finally:
        if args.out and sink is not sys.stdout:
            sink.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
