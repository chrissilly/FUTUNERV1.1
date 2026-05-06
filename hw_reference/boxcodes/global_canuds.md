# Global CAN / UDS Protocol

Everything below is **boxcode-independent** — identical for all supported ECUs.

---

## Physical Layer

| Parameter | Value |
|-----------|-------|
| Standard | CAN 2.0B (ISO 11898-1) |
| Baud rate | 500 kbps |
| Frame IDs | Standard 11-bit |
| TX pin | GPIO 17 |
| RX pin | GPIO 18 |
| TX timeout | 100 ms |
| Filter | Accept-all |

## CAN Identifiers

| ID | Direction | Usage |
|----|-----------|-------|
| `0x7E0` | Dongle → ECU | All requests |
| `0x7E8` | ECU → Dongle | All responses |
| `0x7DF` | Dongle → All | Functional broadcast (defined, unused) |

---

## ISO-TP (ISO 15765-2)

Fragments UDS messages across multiple CAN frames.

| Frame Type | Nibble | Purpose |
|-----------|--------|---------|
| Single Frame | `0x0` | ≤ 7 bytes, length in low nibble |
| First Frame | `0x1` | Start multi-frame, 12-bit total length |
| Consecutive Frame | `0x2` | Continuation, 4-bit sequence counter |
| Flow Control | `0x3` | Receiver → sender: block size, timing |

| Buffer | Size |
|--------|------|
| RX | 4096 bytes |
| TX | 4096 bytes |

---

## UDS Services (ISO 14229-1)

### Service IDs

| SID | Name | Response SID |
|-----|------|-------------|
| `0x3E` | Tester Present | `0x7E` |
| `0x22` | Read Data By Identifier | `0x62` |
| `0x7F` | Negative Response | — |

### Negative Response Codes

| NRC | Name |
|-----|------|
| `0x10` | General Reject |
| `0x11` | Service Not Supported |
| `0x12` | Sub-Function Not Supported |
| `0x13` | Incorrect Message Length |
| `0x22` | Conditions Not Correct |
| `0x31` | Request Out Of Range |
| `0x33` | Security Access Denied |
| `0x78` | Response Pending |

---

## Connection Sequence

All timings and commands below are the same for every boxcode.

### 1. Discovery

```
TX: 3E 00          (Tester Present, normal)
OK: 7E 00          (positive)
```
Retry every 3000 ms until response.

### 2. Read VIN

```
TX: 22 F1 90       (Read DID 0xF190)
OK: 62 F1 90 [17 bytes ASCII VIN]
```
Compared against NVS-stored VIN for pairing check.

### 3. Read Boxcode (Serial Number)

```
TX: 22 F1 87       (Read DID 0xF187)
OK: 62 F1 87 [variable-length ASCII]
```
Example: `4K0907557G__0003`. This selects the boxcode config.

### 4. Read Software Version

```
TX: 22 F1 89       (Read DID 0xF189)
OK: 62 F1 89 [variable-length ASCII]
```
Stored and displayed. **Not used for any logic.**

### 5. Read Build ID

```
TX: 22 F1 F4       (Read DID 0xF1F4)
OK: 62 F1 F4 [variable-length ASCII]
```
Stored and displayed. **Not used for any logic.**

### 6. Check Patch Status

```
TX: 3E 39 01       (Tester Present, subfunction 0x39, param 0x01)
OK: 7E ...         → ECU is PATCHED (logger available)
NG: 7F 3E 12       → ECU is STOCK (no logger)
```

### 7. Get Patch Info (patched only)

```
TX: 3E 01 80       (3 bytes)
OK: 7E [ver] [buf_hi] [buf_lo] [pad] [pad] [addr3] [addr2] [addr1] [addr0]
```

| Byte | Field | Typical Value |
|------|-------|---------------|
| 1 | patch_version | `0x01` (V2) |
| 2–3 | buffer_size | `0x03FF` (1023) |
| 6–9 | log_buffer_address | 32-bit BE |

### 8. Configure Logger (patched only)

Variable-length ISO-TP message listing requested variable addresses and sizes. Sent once after patch info is received.

### 9. Steady State

| ECU State | Behavior |
|-----------|----------|
| Stock | Keep-alive `3E 00` every 1000 ms |
| Patched, engine off | Poll logger every 1000 ms |
| Patched, engine on (`nmot_w > 0`) | Poll logger immediately (continuous, ~100 Hz) |

---

## ECU Write Protocol

Used by the `write_ecu` WebSocket command. Requires authentication.

### Request Format

```
Byte 0:    0x3E           (Tester Present SID)
Byte 1:    0x39           (RAM write subfunction)
Byte 2:    0x01           (fixed marker)
Byte 3:    mid_byte       (boxcode-specific: 0x80 or 0x09)
Byte 4–6:  address        (24-bit, after boxcode offset applied)
Byte 7+:   data           (up to 64 bytes)
```

### Response Handling

| Response | Meaning | Next Action |
|----------|---------|-------------|
| `7E 01` | Chunk accepted | Send next chunk (or done) |
| `7E 05` | Not ready | Send follow-up: `02 3E 37` |
| `7F ...` | Error | Abort, report to client |

### Follow-Up (after "not ready")

```
TX: 02 3E 37
OK: 7E 01 [status] [mid_byte_echo] [addr_echo...]
```
Validates mid_byte and address echo before proceeding.

### Chunking

Data > 64 bytes is split automatically. Each chunk increments the target address by 64.

---

## ISO-TP Channel Arbitration

Only one subsystem uses the `0x7E0`/`0x7E8` channel at a time.

| Owner | Blocking | Use Case |
|-------|----------|----------|
| Connection Manager | Yes (1 s timeout) | Discovery, DIDs, keepalive |
| Logger | Non-blocking (skip if busy) | Periodic variable polling |
| ECU Write | Holds until complete | All chunks in one session |

---

## Timeouts

| Timeout | Value |
|---------|-------|
| Discovery retry | 3000 ms |
| UDS response | 1000 ms |
| Keep-alive interval | 1000 ms |
| CAN TX per-frame | 100 ms |

---

## Connection Status Values

| Status | Meaning |
|--------|---------|
| `DISCONNECTED` | No ECU communication |
| `CONNECTING` | Discovery / identification in progress |
| `CONNECTED_UNPATCHED` | Stock ECU — keepalive only |
| `CONNECTED_PATCHED` | Patched ECU — logger active |
| `ERROR` | Communication failure |

---

## DID Summary

| DID | Hex Bytes | Content | Used For |
|-----|-----------|---------|----------|
| `0xF190` | `F1 90` | 17-char VIN | Vehicle identification, pairing |
| `0xF187` | `F1 87` | Boxcode string | Variable table + write param lookup |
| `0xF189` | `F1 89` | Software version | Display only |
| `0xF1F4` | `F1 F4` | Build ID | Display only |

## All CAN Messages Sent

| Bytes | When | Purpose |
|-------|------|---------|
| `3E 00` | Discovery / keep-alive | Find ECU, prevent timeout |
| `22 F1 90` | Connection | Read VIN |
| `22 F1 87` | Connection | Read boxcode |
| `22 F1 89` | Connection | Read software version |
| `22 F1 F4` | Connection | Read build ID |
| `3E 39 01` | Connection | Check patch status |
| `3E 01 80` | Connection (patched) | Get patch buffer info |
| `3E 39 01 [mid] [addr] [data]` | On command | Write ECU RAM |
| `02 3E 37` | Write retry | Follow-up after "not ready" |
| *variable-length* | Connection (patched) | Logger configuration |
| *variable-length* | Polling (patched) | Logger data request |
