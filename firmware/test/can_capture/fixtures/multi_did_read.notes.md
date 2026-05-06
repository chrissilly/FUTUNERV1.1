# Fixture: multi_did_read

## What this represents

A standard UDS ReadDataByIdentifier request that asks for **two DIDs
in a single message** — `0xF190` (VIN) followed by `0xF187`
(Vehicle Manufacturer Spare Part Number / boxcode). The ECU responds
with both data values concatenated in the standard ISO 14229
multi-DID response format:

```
62 [DID_hi DID_lo data]+
```

where the parser must use known per-DID lengths to split the response
correctly (the wire format does not carry per-DID lengths). VIN is
fixed at 17 ASCII characters (ISO 15031-6); boxcode is variable but
in this fixture is a 16-char string (`4K0907557G__0003`, the same
boxcode used as the canonical example throughout
`docs/CAN_UDS_PROTOCOL.md`).

The total response payload is 38 bytes:
`62 F1 90 [17B VIN] F1 87 [16B boxcode]`. That spills over a single
CAN frame, so the response is delivered as ISO-TP First Frame +
five Consecutive Frames with a tester Flow Control in between.

## CAN frames in the fixture

```
(ts=...000100) can0 7E0#0522F190F1870000
        Tester → ECU. Single frame (PCI 0x05 = 5 data bytes).
        Service 0x22, DIDs 0xF190 0xF187.

(ts=...001500) can0 710#102662F190574155
        ECU → Tester. First frame (PCI 0x10, length 0x026 = 38 bytes).
        Payload bytes (6): 62 F1 90 57 41 55.

(ts=...002100) can0 7E0#3000000000000000
        Tester → ECU. Flow control (continue, BS=0, ST=0).

(ts=...002800) can0 710#215A5A5A344D3132    CF1: 5A 5A 5A 34 4D 31 32
(ts=...003400) can0 710#2232303030313233    CF2: 32 30 30 30 31 32 33
(ts=...004000) can0 710#23F187344B303930    CF3: F1 87 34 4B 30 39 30
(ts=...004600) can0 710#2437353537475F5F    CF4: 37 35 35 37 47 5F 5F
(ts=...005200) can0 710#2530303033000000    CF5: 30 30 30 33 (+3 pad)
```

Total reassembled response = 6 + 7×4 + 4 = 38 bytes, matching the
FF's declared length of `0x026`.

## Expected reassembly

```
62 F1 90 57 41 55 5A 5A 5A 34 4D 31 32 32 30 30 30 31 32 33 \
   F1 87 34 4B 30 39 30 37 35 35 37 47 5F 5F 30 30 30 33
```

Decoded:
- `62` — positive response to `0x22`.
- `F1 90` + 17 bytes — VIN `WAUZZZ4M122000123`.
- `F1 87` + 16 bytes — Boxcode `4K0907557G__0003`.

## What this fixture validates in the parser

1. **Multi-DID request decoding** — the request line carries two DIDs;
   the parser reports both in `did` as a comma-joined string
   (`"0xF190,0xF187"`).
2. **Multi-DID response decoding** — the response payload is split per
   the parser's known-DID length table (VIN = 17, boxcode F187 = "rest").
   Putting VIN first matters: the parser only handles multi-DID
   responses correctly when each non-final DID has a known fixed
   length. A future fixture should cover the harder case where the
   first DID is variable-length (currently a known parser limitation,
   noted in `parse_uds.py` near `_decode_read_data_by_id`).
3. **Multi-frame ISO-TP reassembly across more than two CFs** —
   read_vin only had two CFs; this one has five, exercising the
   sequence-number wrap-relevant code path (sequences `0x21`
   through `0x25`).
4. **Per-DID decoded payload formatting** — when more than one DID is
   present, `decoded` becomes `"0xF190=..., 0xF187=..."` instead of
   the bare value used in the single-DID case. That's a deliberate
   schema choice so a downstream tool can parse it back deterministically.

## What this fixture does NOT validate

- Multi-DID response with the first DID being variable-length
  (parser eats the rest into that DID — known limitation).
- Sequence-number wrap from `0x2F` back to `0x20` (would need a
  >100-byte payload).
- Mixed positive + negative response in one DID list.
