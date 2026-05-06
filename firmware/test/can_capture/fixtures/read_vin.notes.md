# Fixture: read_vin

## What this represents

A standard UDS ReadDataByIdentifier(0xF190) sequence — "tester reads
the VIN from the ECU." This is the simplest non-trivial UDS exchange
and exercises the parser's ISO-TP reassembly path because the ECU
response is a multi-frame message.

## CAN frames in the fixture

```
(ts=...000123) can0 7E0#0322F19000000000
        Tester → ECU. Single frame (ISO-TP PCI 0x03 = 3 data bytes).
        Service 0x22, DID 0xF190.

(ts=...001500) can0 710#101462F190574155
        ECU → Tester. First frame (ISO-TP PCI 0x10, length nibble 0x0,
        length byte 0x14 = 20 bytes total payload coming).
        Payload bytes (6): 62 F1 90 57 41 55.

(ts=...002100) can0 7E0#3000000000000000
        Tester → ECU. Flow control (PCI 0x30 = continue,
        BS=0 = no separation, ST=0).

(ts=...002800) can0 710#215A5A5A344D3132
        ECU → Tester. Consecutive frame, sequence number 1 (PCI 0x21).
        Payload bytes (7): 5A 5A 5A 34 4D 31 32.

(ts=...003400) can0 710#2232303030313233
        ECU → Tester. Consecutive frame, sequence number 2 (PCI 0x22).
        Payload bytes (7): 32 30 30 30 31 32 33.
```

All frames are well-formed CAN-classic (8 data bytes per frame, no
over-DLC).

## Expected reassembly

The ISO-TP layer assembles the response payload across the three ECU
frames in order: 6 bytes from FF + 7 from CF1 + 7 from CF2 = 20 bytes,
matching the FF's declared length of 0x014.

```
62 F1 90 57 41 55 5A 5A 5A 34 4D 31 32 32 30 30 30 31 32 33
```

Which decodes as:

- `62` — positive response to service `0x22` (response service ID is
  the request ID + 0x40).
- `F1 90` — the DID being responded to.
- `57 41 55 5A 5A 5A 34 4D 31 32 32 30 30 30 31 32 33` — the VIN as
  ASCII bytes: `"WAUZZZ4M122000123"` (a synthetic Audi-style VIN; not
  a real vehicle).

## What this fixture validates in the parser

1. Single-frame ISO-TP request decoding (the `0x03 22 F1 90` frame).
2. Multi-frame ISO-TP response reassembly (first frame + 2 consecutive
   frames, exact 20-byte payload boundary).
3. UDS service decoding (0x22 → "ReadDataByIdentifier", 0x62 →
   "ReadDataByIdentifier (positive response)").
4. DID extraction from both request and response.
5. Optional ASCII decoding of the data payload when the parser knows
   the DID is a string-valued identifier (F190 = VIN per ISO 15031-6).

## What this fixture does NOT validate

- Negative response (0x7F) handling.
- Multi-DID requests.
- Routine activation, security access, or any non-read service.
- Per-variant address-specific decoding.

Future fixtures will cover those cases. For the bench toolkit's
initial deliverable, the read_vin fixture is the contract.

## Hand-verification of byte alignment

If you're checking the fixture by eye, the FF declares 20 bytes total
payload. CF sequence numbers go 0x21, 0x22, 0x23, ... — each carrying
up to 7 payload bytes. So a 20-byte payload = 6 (FF) + 7 (CF1) + 7
(CF2) = 20 exactly, which is what this fixture has. No padding is
needed and the over-DLC errors that an earlier draft of this fixture
contained have been corrected.
