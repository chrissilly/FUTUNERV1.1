# Fixture: negative_response

## What this represents

A standard UDS request that the ECU rejects with a Negative Response
(service `0x7F`). Specifically: tester reads VIN, ECU refuses with
NRC `0x33` (Security Access Denied) — the canonical case for a
diagnostic request the ECU won't honor without unlock.

This fixture exercises the parser's NR-decoding path, which is
distinct from the positive-response path (`SID + 0x40`) because:

- The service ID is `0x7F`, not `original_SID + 0x40`.
- Byte 1 is the rejected service ID (echoes the original request).
- Byte 2 is the NRC (negative response code), per ISO 14229-1 §A.1.

## CAN frames in the fixture

```
(ts=...000100) can0 7E0#0322F19000000000
        Tester → ECU. Single frame (PCI 0x03 = 3 data bytes).
        Service 0x22 (ReadDataByIdentifier), DID 0xF190 (VIN).

(ts=...001000) can0 710#037F223300000000
        ECU → Tester. Single frame (PCI 0x03 = 3 data bytes).
        Service 0x7F (Negative Response), rejected SID 0x22, NRC 0x33.
```

## Expected parser output

Two JSONL records — one for the request, one for the negative response:

1. The request record matches the read_vin fixture's request line
   exactly (same bytes, same DID).
2. The NR record sets:
   - `service`: `"Negative Response"`
   - `service_id`: `"0x7F"`
   - `did`: `null` (NRs don't carry a DID — the rejected service does,
     but the NR itself is structurally a different message)
   - `raw`: `"7F 22 33"`
   - `decoded`: a human-readable description of the rejected service
     and the NRC, including the NRC's standard name from the table
     in `docs/CAN_UDS_PROTOCOL.md` §"Negative Response Codes (NRCs)".

## What this fixture validates in the parser

1. NR detection (SID == `0x7F`, not the `+0x40` mirror rule).
2. Rejected-service-ID extraction from the NR payload.
3. NRC name lookup via the parser's NRC table.
4. The NR record is emitted as `dir = "rx"` because the source CAN ID
   is the configured ECU response ID (`0x710`).

## What this fixture does NOT validate

- Multi-frame NR (NRs are always SF in practice).
- NRC `0x78` (Response Pending) special semantics — that one means
  "wait, I'll get back to you," and the dongle's stack treats it
  differently from a real failure. A future fixture should cover it.
- NRs that arrive with no rejected-SID byte (malformed); the parser
  is forgiving (uses `0x00` as a placeholder) but no fixture asserts
  that behavior yet.
