"""Unit tests for parse_uds.py.

Run with:
    pytest firmware/test/can_capture/tests/

Independent of the eval harness. The eval grades end-to-end CLI
output against fixtures; this suite tests the parser internals
(line parsing, ISO-TP reassembly, individual UDS decode paths)
in isolation, so a failure here points at a specific function
rather than at the whole pipeline.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

# Make the parser importable as a sibling module without an installed package.
HERE = Path(__file__).resolve().parent
BENCH = HERE.parent / "bench"
sys.path.insert(0, str(BENCH))

import parse_uds  # noqa: E402
from parse_uds import (  # noqa: E402
    CanFrame,
    IsoTpReassembler,
    UdsMessage,
    decode_uds,
    parse_candump_line,
    parse_capture,
)


FIXTURES = HERE.parent / "fixtures"


# ---------------------------------------------------------------------------
# parse_candump_line
# ---------------------------------------------------------------------------

class TestParseCandumpLine:
    def test_single_frame_request(self):
        f = parse_candump_line("(0001714892100.000123) can0 7E0#0322F19000000000")
        assert f is not None
        assert f.ts == 1714892100.000123
        assert f.iface == "can0"
        assert f.can_id == 0x7E0
        assert f.data == bytes.fromhex("0322F19000000000")

    def test_first_frame_response(self):
        f = parse_candump_line("(0001714892100.001500) can0 710#101462F190574155")
        assert f is not None
        assert f.can_id == 0x710
        assert f.data[:2] == b"\x10\x14"

    def test_blank_line_ignored(self):
        assert parse_candump_line("") is None
        assert parse_candump_line("   ") is None

    def test_comment_line_ignored(self):
        assert parse_candump_line("# this is a comment") is None

    def test_garbage_line_ignored(self):
        assert parse_candump_line("not a candump line at all") is None

    def test_odd_nibble_data_rejected(self):
        # Malformed CAN frame — the parser must NOT pad or guess.
        assert parse_candump_line("(0001714892100.000000) can0 710#1014621F19057415A") is None

    def test_empty_data_field_accepted(self):
        # Some captures have data-less status lines; these have no
        # PCI byte, so the reassembler will skip them downstream.
        f = parse_candump_line("(0001714892100.000000) can0 7E0#")
        assert f is not None
        assert f.data == b""


# ---------------------------------------------------------------------------
# IsoTpReassembler
# ---------------------------------------------------------------------------

class TestIsoTpReassembler:
    def _frame(self, ts: float, can_id: int, data_hex: str) -> CanFrame:
        return CanFrame(ts=ts, iface="can0", can_id=can_id, data=bytes.fromhex(data_hex))

    def test_single_frame_emitted_immediately(self):
        r = IsoTpReassembler()
        msg = r.feed(self._frame(1.0, 0x7E0, "0322F19000000000"))
        assert msg is not None
        assert msg.payload == bytes.fromhex("22F190")
        assert msg.ts == 1.0
        assert msg.src_id == 0x7E0

    def test_first_frame_returns_none_until_complete(self):
        r = IsoTpReassembler()
        assert r.feed(self._frame(1.0, 0x710, "101462F190574155")) is None
        assert r.feed(self._frame(2.0, 0x710, "215A5A5A344D3132")) is None
        msg = r.feed(self._frame(3.0, 0x710, "2232303030313233"))
        assert msg is not None
        assert msg.ts == 1.0  # FF timestamp wins
        assert msg.payload == bytes.fromhex("62F190") + b"WAUZZZ4M122000123"
        assert len(msg.payload) == 20  # FF declared length 0x14

    def test_flow_control_emits_nothing(self):
        r = IsoTpReassembler()
        assert r.feed(self._frame(1.0, 0x7E0, "3000000000000000")) is None

    def test_consecutive_frame_with_no_first_frame_dropped(self):
        r = IsoTpReassembler()
        assert r.feed(self._frame(1.0, 0x710, "215A5A5A344D3132")) is None

    def test_out_of_sequence_cf_aborts_reassembly(self):
        r = IsoTpReassembler()
        r.feed(self._frame(1.0, 0x710, "101462F190574155"))
        # Skip CF1, send CF2 → should drop the in-progress reassembly.
        assert r.feed(self._frame(2.0, 0x710, "2232303030313233")) is None
        # CF1 arriving late should ALSO be ignored (no buffer).
        assert r.feed(self._frame(3.0, 0x710, "215A5A5A344D3132")) is None

    def test_per_can_id_isolation(self):
        r = IsoTpReassembler()
        # Tester sends a SF; ECU is mid-FF. The two streams must not
        # interfere because they're keyed on different CAN IDs.
        r.feed(self._frame(1.0, 0x710, "101462F190574155"))
        sf = r.feed(self._frame(1.5, 0x7E0, "0322F19000000000"))
        assert sf is not None
        assert sf.src_id == 0x7E0
        # ECU FF reassembly is still in flight — finish it.
        r.feed(self._frame(2.0, 0x710, "215A5A5A344D3132"))
        ff = r.feed(self._frame(3.0, 0x710, "2232303030313233"))
        assert ff is not None
        assert ff.src_id == 0x710


# ---------------------------------------------------------------------------
# decode_uds
# ---------------------------------------------------------------------------

class TestDecodeUds:
    def test_request_read_data_by_id_single(self):
        msg = UdsMessage(ts=1.0, src_id=0x7E0, payload=bytes.fromhex("22F190"))
        rec = decode_uds(msg)
        assert rec["dir"] == "tx"
        assert rec["src"] == "0x7E0"
        assert rec["dst"] == "0x710"
        assert rec["service"] == "ReadDataByIdentifier"
        assert rec["service_id"] == "0x22"
        assert rec["did"] == "0xF190"
        assert rec["raw"] == "22 F1 90"
        assert rec["decoded"] is None

    def test_request_read_data_by_id_multi(self):
        msg = UdsMessage(ts=1.0, src_id=0x7E0, payload=bytes.fromhex("22F190F187"))
        rec = decode_uds(msg)
        assert rec["did"] == "0xF190,0xF187"

    def test_response_vin_decoded(self):
        payload = bytes.fromhex("62F190") + b"WAUZZZ4M122000123"
        msg = UdsMessage(ts=1.0, src_id=0x710, payload=payload)
        rec = decode_uds(msg)
        assert rec["dir"] == "rx"
        assert rec["service_id"] == "0x62"
        assert rec["did"] == "0xF190"
        assert rec["decoded"] == "WAUZZZ4M122000123"

    def test_response_multi_did_decoded(self):
        payload = (
            bytes.fromhex("62F190")
            + b"WAUZZZ4M122000123"
            + bytes.fromhex("F187")
            + b"4K0907557G__0003"
        )
        msg = UdsMessage(ts=1.0, src_id=0x710, payload=payload)
        rec = decode_uds(msg)
        assert rec["did"] == "0xF190,0xF187"
        assert rec["decoded"] == "0xF190=WAUZZZ4M122000123, 0xF187=4K0907557G__0003"

    def test_negative_response(self):
        msg = UdsMessage(ts=1.0, src_id=0x710, payload=bytes.fromhex("7F2233"))
        rec = decode_uds(msg)
        assert rec["service"] == "Negative Response"
        assert rec["service_id"] == "0x7F"
        assert rec["did"] is None
        assert "ReadDataByIdentifier" in rec["decoded"]
        assert "Security Access Denied" in rec["decoded"]

    def test_negative_response_unknown_nrc(self):
        # NRC 0xAB is not in the table; should fall back to "Unknown NRC".
        msg = UdsMessage(ts=1.0, src_id=0x710, payload=bytes.fromhex("7F22AB"))
        rec = decode_uds(msg)
        assert "0xAB" in rec["decoded"]
        assert "Unknown NRC" in rec["decoded"]

    def test_unknown_service_does_not_crash(self):
        # 0x55 is not in UDS_SERVICES; parser must not raise.
        msg = UdsMessage(ts=1.0, src_id=0x7E0, payload=bytes.fromhex("550102"))
        rec = decode_uds(msg)
        assert rec["service_id"] == "0x55"
        assert "Unknown service" in rec["service"]

    def test_off_pair_traffic_skipped(self):
        # ID outside the configured tester/ECU pair returns None.
        msg = UdsMessage(ts=1.0, src_id=0x123, payload=bytes.fromhex("22F190"))
        assert decode_uds(msg) is None

    def test_tester_present_keepalive(self):
        msg = UdsMessage(ts=1.0, src_id=0x7E0, payload=bytes.fromhex("3E00"))
        rec = decode_uds(msg)
        assert rec["service"] == "TesterPresent"
        assert rec["service_id"] == "0x3E"

    def test_custom_id_pair(self):
        # Standard ISO 14229 pair (0x7E0/0x7E8) instead of VW (0x7E0/0x710).
        msg = UdsMessage(ts=1.0, src_id=0x7E8, payload=bytes.fromhex("62F190") + b"WAUZZZ4M122000123")
        rec = decode_uds(msg, tester_id=0x7E0, ecu_id=0x7E8)
        assert rec["dir"] == "rx"
        assert rec["src"] == "0x7E8"
        assert rec["decoded"] == "WAUZZZ4M122000123"


# ---------------------------------------------------------------------------
# End-to-end against the fixture trios
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "name",
    sorted(p.stem for p in FIXTURES.glob("*.candump")),
)
def test_fixture_end_to_end(name):
    """For every fixture trio, parse_capture should match expected.jsonl."""
    candump = FIXTURES / f"{name}.candump"
    expected_path = FIXTURES / f"{name}.expected.jsonl"
    assert expected_path.exists(), f"missing {expected_path.name}"

    with open(candump) as f:
        actual = [
            {**rec, "ts": "TS"}
            for rec in parse_capture(f)
        ]
    with open(expected_path) as f:
        expected = [
            {**json.loads(line), "ts": "TS"}
            for line in f if line.strip()
        ]
    assert actual == expected, f"fixture {name} mismatch"


def test_defaults_loaded_from_cfg():
    """defaults.cfg must populate the module-level constants."""
    assert parse_uds.DEFAULT_TESTER_ID == 0x7E0
    assert parse_uds.DEFAULT_ECU_ID == 0x710
