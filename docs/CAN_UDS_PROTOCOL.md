# SEFI v2 — CAN Bus & ECU Protocol Reference

Complete reference of every interaction the SEFI dongle has with the vehicle's CAN bus and ECU systems.

---

## Table of Contents

1. [Physical Layer](#1-physical-layer)
2. [CAN Identifiers](#2-can-identifiers)
3. [ISO-TP Transport Layer](#3-iso-tp-transport-layer)
4. [UDS Services Used](#4-uds-services-used)
5. [Connection Sequence](#5-connection-sequence)
6. [ECU Identification DIDs](#6-ecu-identification-dids)
7. [Patch Detection Protocol](#7-patch-detection-protocol)
8. [Logger System](#8-logger-system)
9. [ECU Memory Write Protocol](#9-ecu-memory-write-protocol)
10. [BDEF Files (Binary Definitions)](#10-bdef-files-binary-definitions)
11. [SCAL Files (Calibration Data)](#11-scal-files-calibration-data)
12. [ISO-TP Channel Arbitration](#12-isotp-channel-arbitration)
13. [Supported Boxcodes & Variables](#13-supported-boxcodes--variables)
14. [WebSocket Command Interface](#14-websocket-command-interface)
15. [Quick-Reference Tables](#15-quick-reference-tables)

---

## 1. Physical Layer

| Parameter | Value |
|-----------|-------|
| Bus standard | CAN 2.0B (ISO 11898-1) |
| Baud rate | 500 kbps (fixed) |
| Frame format | Standard 11-bit identifiers |
| TX pin | GPIO 17 |
| RX pin | GPIO 18 |
| Transceiver | On-board (OBD-2025-12-31-4Gb PCB) |
| Filter | Accept-all (no hardware filtering) |
| TX timeout | 100 ms per frame |

The dongle connects to the OBD-II port (pins 6 CAN-H, 14 CAN-L) and communicates at the standard OBD diagnostic baud rate.

---

## 2. CAN Identifiers

| ID | Direction | Name | Purpose |
|----|-----------|------|---------|
| `0x7E0` | Dongle → ECU | Physical TX | All dongle requests to the ECU |
| `0x7E8` | ECU → Dongle | Physical RX | All ECU responses to the dongle |
| `0x7DF` | Dongle → All | Functional TX | Broadcast to all ECUs (defined but unused in current code) |

These are standard OBD-II diagnostic CAN IDs for the primary ECU (engine controller). The dongle only talks to **one ECU** at a time on the physical addressing pair `0x7E0`/`0x7E8`.

---

## 3. ISO-TP Transport Layer

**Standard:** ISO 15765-2

CAN frames carry max 8 bytes. ISO-TP fragments larger UDS messages across multiple frames.

### Frame Types

| Type | First Nibble | Purpose |
|------|-------------|---------|
| Single Frame (SF) | `0x0` | Messages ≤ 7 bytes — length in low nibble |
| First Frame (FF) | `0x1` | Start of multi-frame transfer — 12-bit total length |
| Consecutive Frame (CF) | `0x2` | Continuation — 4-bit sequence counter (wraps 0–F) |
| Flow Control (FC) | `0x3` | Receiver tells sender: status, block size, min separation time |

### Buffer Configuration

| Parameter | Value |
|-----------|-------|
| RX buffer | 4096 bytes |
| TX buffer | 4096 bytes |
| Max message size | 4096 bytes per direction |

### Library

Uses `isotp-c` (open source) with ESP-IDF shims:
- `isotp_user_send_can()` → calls `can_driver_send()`
- `isotp_user_get_ms()` → FreeRTOS tick count
- `isotp_user_debug()` → ESP_LOGD

---

## 4. UDS Services Used

**Standard:** ISO 14229-1 (Unified Diagnostic Services)

| Service ID | Name | Positive Response | Usage |
|-----------|------|-------------------|-------|
| `0x3E` | Tester Present | `0x7E` | Keep-alive, patch detection, live mode, RAM write |
| `0x22` | Read Data By Identifier | `0x62` | Read VIN, boxcode, software version, build ID |
| `0x7F` | Negative Response | — | Error indicator (always from ECU) |

### Negative Response Codes (NRCs)

| NRC | Name | Meaning |
|-----|------|---------|
| `0x10` | General Reject | ECU refuses request |
| `0x11` | Service Not Supported | ECU doesn't implement this service |
| `0x12` | Sub-Function Not Supported | Valid service, unknown subfunction |
| `0x13` | Incorrect Message Length | Wrong payload size |
| `0x22` | Conditions Not Correct | Preconditions not met |
| `0x31` | Request Out Of Range | Invalid address or DID |
| `0x33` | Security Access Denied | Not authorized |
| `0x78` | Response Pending | ECU busy, response coming later |

---

## 5. Connection Sequence

The dongle follows a strict state machine from power-on to fully connected. Every arrow is one ISO-TP request/response pair.

```
 BOOT
  │
  ▼
 ┌─────────────────────────┐
 │ DISCOVERING              │  TX: 3E 00 (Tester Present, normal)
 │ Retry every 3 s          │  Expect: 7E 00 (positive) or timeout
 └────────────┬─────────────┘
              │ Got positive response
              ▼
 ┌─────────────────────────┐
 │ REQUEST_VIN              │  TX: 22 F1 90 (ReadDataByID: VIN)
 │                          │  Expect: 62 F1 90 [17 bytes]
 └────────────┬─────────────┘
              │ Got VIN
              ▼
 ┌─────────────────────────┐
 │ CHECK_PAIRING            │  Compare VIN with NVS-stored VIN
 │                          │  Match → skip to CHECK_PATCH_STATUS
 │                          │  No match → REQUEST_SERIAL
 └────────────┬─────────────┘
              │ New vehicle (no match)
              ▼
 ┌─────────────────────────┐
 │ REQUEST_SERIAL           │  TX: 22 F1 87 (ReadDataByID: Boxcode)
 │                          │  Expect: 62 F1 87 [boxcode string]
 │                          │  e.g. "4K0907557G__0003"
 └────────────┬─────────────┘
              ▼
 ┌─────────────────────────┐
 │ REQUEST_SOFTWARE_VERSION │  TX: 22 F1 89
 │                          │  Expect: 62 F1 89 [version string]
 └────────────┬─────────────┘
              ▼
 ┌─────────────────────────┐
 │ REQUEST_BUILD_ID         │  TX: 22 F1 F4
 │                          │  Expect: 62 F1 F4 [build ID string]
 └────────────┬─────────────┘
              ▼
 ┌─────────────────────────┐
 │ CHECK_PATCH_STATUS       │  TX: 3E 39 01 (Tester Present, live mode)
 │                          │  Positive (7E) → ECU is PATCHED
 │                          │  Negative (7F) → ECU is STOCK
 └───────┬──────────┬───────┘
         │          │
    PATCHED      STOCK
         │          │
         ▼          ▼
 ┌──────────┐  ┌───────────────┐
 │ REQUEST   │  │ CONNECTED     │
 │ PATCH_INFO│  │ (unpatched)   │
 │           │  │ Keep-alive    │
 │ TX: 3E    │  │ every 1000ms: │
 │   01 80   │  │ TX: 3E 00     │
 └─────┬─────┘  └───────────────┘
       │
       │ Got patch info (buffer addr, version, size)
       ▼
 ┌─────────────────────────┐
 │ CONFIGURE_LOGGER         │  Send variable list to ECU
 │                          │  (addresses, sizes, scaling)
 └────────────┬─────────────┘
              ▼
 ┌─────────────────────────┐
 │ CONNECTED (patched)      │
 │ → POLLING_LOGGER         │  Poll loop: request → parse → repeat
 │   Engine on:  ~100 Hz    │
 │   Engine off: 1 Hz       │
 └──────────────────────────┘
```

### Timeouts

| Phase | Timeout |
|-------|---------|
| Discovery retry | 3000 ms |
| UDS request response | 1000 ms |
| Keep-alive interval | 1000 ms |

---

## 6. ECU Identification DIDs

| DID | Hex | Service | Response Format | Example |
|-----|-----|---------|-----------------|---------|
| VIN | `0xF190` | `22 F1 90` → `62 F1 90 [17B]` | 17-char ASCII | `WAUZZZ4K9MN012345` |
| Serial / Boxcode | `0xF187` | `22 F1 87` → `62 F1 87 [str]` | Variable-length ASCII | `4K0907557G__0003` |
| Software Version | `0xF189` | `22 F1 89` → `62 F1 89 [str]` | Variable-length ASCII | `0040` |
| Build ID | `0xF1F4` | `22 F1 F4` → `62 F1 F4 [str]` | Variable-length ASCII | `SC800-2.7.1-2339-B` |

The **boxcode** (DID `0xF187`) is the key lookup — it determines which variable table, write parameters, and address offsets apply.

---

## 7. Patch Detection Protocol

The dongle distinguishes between stock and patched ECUs using a custom subfunction of Tester Present.

### Step 1: Check Patch

```
TX:  3E 39 01        (Tester Present, subfunction 0x39, param 0x01)
```

| Response | Meaning |
|----------|---------|
| `7E ...` (positive) | ECU has SEFI logger patch installed |
| `7F 3E 12` (negative, subfunction not supported) | ECU is stock |

### Step 2: Request Patch Info (patched only)

```
TX:  3E 01 80        (3 bytes raw)
```

**Response format** (≥ 11 bytes):

```
Byte  0:    0x7E              (positive response to 0x3E)
Byte  1:    patch_version     (expected: 0x01 = V2)
Byte  2-3:  buffer_size       (big-endian, typically 1023)
Byte  4-5:  padding
Byte  6-9:  log_buffer_addr   (big-endian, 32-bit ECU RAM address)
```

The `log_buffer_address` is where the ECU patch writes live variable data. The dongle reads from this address during logger polling.

---

## 8. Logger System

The logger is only available on **patched** ECUs. It provides live streaming of internal ECU variables at up to ~100 Hz.

### How It Works

1. The ECU patch reserves a 1023-byte RAM buffer at `log_buffer_address`
2. The dongle sends a configuration message listing which variables to capture
3. The ECU patch starts copying requested variable values into the buffer
4. The dongle polls the buffer periodically and decodes the raw bytes

### Variable Definition Format

Each logged variable has:

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Internal ID (e.g. `nmot_w`) |
| `display_name` | string | Human-readable (e.g. "Engine Speed") |
| `unit` | string | Physical unit (e.g. "rpm") |
| `address` | uint32 | ECU RAM address |
| `size` | uint8 | 1 or 2 bytes |
| `scale` | float | Raw × scale = physical value |
| `offset` | float | Added after scaling |
| `is_signed` | bool | Interpret as signed integer |
| `is_required` | bool | Always included in logger config |

### Value Conversion

```
physical_value = (raw_bytes × scale) + offset
```

Example: Engine RPM
```
raw = 0x0C80 (3200 in uint16)
scale = 0.25
physical = 3200 × 0.25 = 800.0 rpm
```

### Polling Frequency

| Engine State | Poll Interval | Effective Rate |
|-------------|---------------|----------------|
| Running (`nmot_w > 0`) | 0 ms (immediate) | ~100 Hz (limited by bus) |
| Stopped (`nmot_w == 0`) | 1000 ms | 1 Hz |

### Logger Limits

| Parameter | Value |
|-----------|-------|
| Max variables | 32 |
| Config buffer | 256 bytes |
| ECU patch buffer | 1023 bytes |
| Max variable size | 2 bytes |

---

## 9. ECU Memory Write Protocol

The dongle can write arbitrary data to ECU RAM. This is used for calibration changes (ethanol content, speed display, map switching).

**Security:** Requires WebSocket authentication (`write_ecu` is `CMD_SECURITY_SECURED`).

### Write Request Format

```
Byte 0:      0x3E              (Tester Present service ID)
Byte 1:      0x39              (RAM write subfunction)
Byte 2:      0x01              (fixed marker byte)
Byte 3:      mid_byte          (boxcode-specific: 0x80 or 0x09)
Byte 4-6:    address           (24-bit, with boxcode offset applied)
Byte 7+:     data              (up to 64 bytes per chunk)
```

### Response Handling

| Response | Meaning | Action |
|----------|---------|--------|
| `7E 01` | Chunk accepted | Send next chunk |
| `7E 05` | Not ready | Send follow-up: `02 3E 37` |
| `7F ...` | Error | Abort write, report error |

### Follow-Up After "Not Ready"

```
TX:  02 3E 37        (3 bytes)
```

Expected response:
```
7E 01 [status] [mid_byte_echo] [addr_echo...]
```

The dongle validates the mid_byte and address echo match the original request before proceeding.

### Chunking

Large writes are split into 64-byte chunks automatically:

```
Total data: 256 bytes
  Chunk 1: addr + 0x00, 64 bytes
  Chunk 2: addr + 0x40, 64 bytes
  Chunk 3: addr + 0x80, 64 bytes
  Chunk 4: addr + 0xC0, 64 bytes
```

### Write State Machine

```
IDLE → SENDING_CHUNK → WAIT_CHUNK_RESPONSE
                            │
                     ┌──────┴──────┐
                     │             │
              Got 0x7E 0x01   Got 0x7E 0x05
              (accepted)      (not ready)
                     │             │
                     │             ▼
                     │      SENDING_FOLLOWUP → WAIT_FOLLOWUP_RESPONSE
                     │             │
                     ▼             ▼
              More chunks?    Got followup OK
                     │             │
              ┌──────┴──────┐      │
              Yes           No     │
              │             │      │
              ▼             ▼      │
        SENDING_CHUNK  COMPLETE ◄──┘
```

### Boxcode-Specific Write Parameters

| Boxcode | mid_byte | address_offset | Notes |
|---------|----------|---------------|-------|
| `4K0907557G__0003` | `0x80` | `0x000000` | Direct addressing |
| `8W0907559H__0008` | `0x09` | `0x040000` | 256KB offset applied |
| `4M0906014__0005` | `0x80` | `0x000000` | Direct addressing |
| `4M0906014B__0003` | `0x80` | `0x000000` | Direct addressing |

The `address_offset` is added to every target address before transmission. The dongle handles this transparently.

---

## 10. BDEF Files (Binary Definitions)

BDEF files define **what** to write to the ECU — they contain binary data segments and their target addresses. Stored on the dongle's LittleFS filesystem (`/cal` partition).

### File Signature

```
Bytes 0–3: 0x42 0x44 0x45 0x46  ("BDEF")
Version:   1
```

### Header (32 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | signature | `0x46454442` ("BDEF" reversed) |
| 0x04 | 4 | version | Must be 1 |
| 0x08 | 4 | segment_count | Number of write segments |
| 0x0C | 4 | inverse_segment_count | Number of inverse (undo) segments |
| 0x10 | 4 | pre_cal_data_size | Pre-calibration data block size |
| 0x14 | 4 | post_cal_data_size | Post-calibration data block size |
| 0x18 | 4 | cal_start | Calibration region start address |
| 0x1C | 4 | cal_end | Calibration region end address |

### Segment Index Entry (12 bytes each)

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 4 | target address (ECU RAM) |
| 0x04 | 4 | data size (bytes) |
| 0x08 | 4 | data offset (within file) |

### File Layout

```
[Header 32B] [Segment Index] [Inverse Index] [Segment Data] [Inverse Data] [Pre-Cal] [Post-Cal]
```

### Usage

BDEF segments are read one at a time and written to the ECU using the write protocol (Section 9). The inverse segments can undo a write (restore original data). Max segment size: 64 bytes.

---

## 11. SCAL Files (Calibration Data)

SCAL files define **flex fuel calibration maps** — they contain gasoline maps, ethanol maps, and blend interpolation data for real-time fuel adaptation.

### File Signature

```
Bytes 0–3: 0x53 0x43 0x41 0x4C  ("SCAL")
Version:   0x00000001
```

### Header (1024 bytes / 0x400)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | signature | `0x5343414C` |
| 0x04 | 4 | version | `0x00000001` |
| 0x08 | 4 | total_size | Entire file size |
| 0x0C | 4 | calibration_region_size | Total cal data |
| 0x10 | 4 | flex_map_index_offset | Offset to flex map table |
| 0x14 | 4 | flex_map_index_size | Size of flex map table |
| 0x18 | 4 | custom_table_offset | Offset to custom tables |
| 0x1C | 4 | custom_table_size | Custom table region size |
| 0x20 | 4 | gasoline_region_offset | Offset to gasoline cal data |
| 0x24 | 4 | ethanol_region_offset | Offset to ethanol cal data |

### Flex Map Entry (32 bytes each)

| Field | Size | Description |
|-------|------|-------------|
| original_address | 4 | ECU address to write |
| gasoline_address | 4 | Offset into gasoline region |
| ethanol_address | 4 | Offset into ethanol region |
| blend_map_address | 4 | Offset into custom table |
| x_dimension | 4 | Map X axis size |
| y_dimension | 4 | Map Y axis size |
| byte_order | 4 | 0 = little-endian, 1 = big-endian |
| data_type | 4 | 0–5 (see below) |

### Data Types

| Value | Type | Size |
|-------|------|------|
| 0 | UINT8 | 1 byte |
| 1 | UINT16 | 2 bytes |
| 2 | UINT32 | 4 bytes |
| 3 | INT8 | 1 byte |
| 4 | INT16 | 2 bytes |
| 5 | INT32 | 4 bytes |

### Region Layout

```
┌────────────────────────────────┐
│ Header (1 KB)                  │
├────────────────────────────────┤
│ Flex Map Index (64 KB)         │  Up to 2047 entries × 32 bytes
├────────────────────────────────┤
│ Custom Table Region (500 KB)   │  Blend maps and user modifications
├────────────────────────────────┤
│ Gasoline Calibration Data      │  Stock calibration maps for gasoline
├────────────────────────────────┤
│ Ethanol Calibration Data       │  Modified maps for E85
└────────────────────────────────┘
```

### Flex Fuel Operation

The SCAL file enables real-time fuel adaptation:

1. Read current ethanol percentage from logger (`InjSys_ratEthPrtnBascFu`)
2. For each flex map entry:
   - Read gasoline calibration value at `gasoline_address`
   - Read ethanol calibration value at `ethanol_address`
   - Interpolate between them using the blend map (9-point curve)
   - Write the blended value to `original_address` in ECU RAM
3. Repeat continuously as ethanol percentage changes

---

## 12. ISO-TP Channel Arbitration

Only one subsystem can use the ISO-TP channel (`0x7E0` / `0x7E8`) at a time. The coordinator prevents collisions.

### Channel Owners

| Owner | Priority | Blocking? | Use Case |
|-------|----------|-----------|----------|
| `CONNECTION_MANAGER` | Normal | Yes (1000ms timeout) | UDS discovery, VIN, keepalive |
| `LOGGER` | Low | Non-blocking (0ms) | Skips poll if channel busy |
| `ECU_WRITE` | High | Yes (acquires and holds) | Holds until all chunks complete |

### Arbitration Rules

- Only ONE owner at a time (mutex-protected)
- Logger requests are **non-blocking**: if channel is busy, the poll is simply skipped
- ECU writes **acquire and hold** the channel for the entire write operation (all chunks)
- Connection manager uses standard blocking requests with timeout

---

## 13. Supported Boxcodes & Variables

### Boxcode: `4K0907557G__0003`

**Endianness:** Little-endian | **MID byte:** `0x80` | **Address offset:** `0x000000`

| Variable | Display Name | Address | Size | Scale | Offset | Unit | Signed |
|----------|-------------|---------|------|-------|--------|------|--------|
| `nmot_w` | Engine Speed | `0x60020618` | 2 | 0.25 | 0 | rpm | No |
| `InjSys_ratEthPrtnBascFu` | Ethanol Content | `0x6001522A` | 2 | 0.00152587 | 0 | % | No |
| `rlp_w` | Predicted Load | `0x5001CB0C` | 2 | 0.0234375 | 0 | % | No |
| `Com_stCrCtlPan` | Cruise Control | `0x600206F8` | 2 | 1.0 | 0 | — | No |
| `rl_w` | Actual Load | `0x60015660` | 2 | 0.0234375 | 0 | % | No |
| `tmot` | Coolant Temp | `0x6001BF38` | 1 | 0.7498 | -48 | °C | No |
| `wdkba` | Throttle Position | `0x6001B842` | 1 | 0.3921 | 0 | % | No |
| `pvdg_w` | Boost Pressure | `0x5001BA86` | 2 | 0.0781 | 0 | mbar | No |
| `zwoutzyl_w` | Ignition Timing | `0x5001CD84` | 2 | 0.1 | 0 | °BTDC | Yes |
| `frm_w` | Fuel Trim | `0x5001C9A6` | 2 | 3.05e-05 | 0 | — | No |

**Special addresses:**
- Ethanol memory: `0x11E6AE`
- Speed display memory: `0x9F93E`

---

### Boxcode: `8W0907559H__0008`

**Endianness:** Big-endian | **MID byte:** `0x09` | **Address offset:** `0x040000`

| Variable | Display Name | Address | Size | Scale | Offset | Unit | Signed |
|----------|-------------|---------|------|-------|--------|------|--------|
| `nmot_w` | Engine Speed | `0x600217D8` | 2 | 0.25 | 0 | rpm | No |
| `InjSys_ratEthPrtnBascFu` | Ethanol Content | `0x600163D2` | 2 | 0.00152587 | 0 | % | No |
| `rlp_w` | Predicted Load | `0x5001DD58` | 2 | 0.0234375 | 0 | % | No |
| `rl_w` | Actual Load | `0x60016808` | 2 | 0.0234375 | 0 | % | No |
| `tmot` | Coolant Temp | `0x6001D0E0` | 1 | 0.7498 | -48 | °C | No |
| `wdkba` | Throttle Position | `0x6001C9EA` | 1 | 0.3921 | 0 | % | No |
| `pvdg_w` | Boost Pressure | `0x5001CC78` | 2 | 0.0781 | 0 | mbar | No |
| `zwoutzyl_w` | Ignition Timing | `0x5001DFD0` | 2 | 0.1 | 0 | °BTDC | Yes |
| `frm_w` | Fuel Trim | `0x5001DBF2` | 2 | 3.05e-05 | 0 | — | No |

**Special addresses:**
- Ethanol memory: `0x6A8550`
- Speed display memory: `0x651F30`

---

### Boxcode: `4M0906014__0005`

**Endianness:** Little-endian | **MID byte:** `0x80` | **Address offset:** `0x000000`

| Variable | Display Name | Address | Size | Scale | Offset | Unit |
|----------|-------------|---------|------|-------|--------|------|
| `nmot_w` | Engine Speed | `0x6001FA78` | 2 | 0.25 | 0 | rpm |
| `rl_w` | Actual Load | `0x60014AC0` | 2 | 0.0234375 | 0 | % |
| `tmot` | Coolant Temp | `0x6001B398` | 1 | 0.7498 | -48 | °C |
| `wdkba` | Throttle Position | `0x6001ACA2` | 1 | 0.3921 | 0 | % |

---

### Boxcode: `4M0906014B__0003`

**Endianness:** Little-endian | **MID byte:** `0x80` | **Address offset:** `0x000000`

| Variable | Display Name | Address | Size | Scale | Offset | Unit | Signed |
|----------|-------------|---------|------|-------|--------|------|--------|
| `nmot_w` | Engine Speed | `0x60020618` | 2 | 0.25 | 0 | rpm | No |
| `InjSys_ratEthPrtnBascFu` | Ethanol Content | `0x6001522A` | 2 | 0.00152587 | 0 | % | No |
| `rlp_w` | Predicted Load | `0x5001CB0C` | 2 | 0.0234375 | 0 | % | No |
| `Com_stCrCtlPan` | Cruise Control | `0x600206F8` | 2 | 1.0 | 0 | — | No |
| `rl_w` | Actual Load | `0x60015660` | 2 | 0.0234375 | 0 | % | No |
| `tmot` | Coolant Temp | `0x6001BF38` | 1 | 0.7498 | -48 | °C | No |
| `wdkba` | Throttle Position | `0x6001B842` | 1 | 0.3921 | 0 | % | No |
| `pvdg_w` | Boost Pressure | `0x5001BA86` | 2 | 0.0781 | 0 | mbar | No |
| `zwoutzyl_w` | Ignition Timing | `0x5001CD84` | 2 | 0.1 | 0 | °BTDC | Yes |
| `frm_w` | Fuel Trim | `0x5001C9A6` | 2 | 3.05e-05 | 0 | — | No |

---

## 14. WebSocket Command Interface

All user interaction goes through WebSocket at `ws://192.168.10.1/ws`. Commands are JSON.

### ECU-Related Commands

| Command | Auth Required | Description |
|---------|:---:|-------------|
| `get_status` | No | Returns connection state, VIN, boxcode, patch status, logger running |
| `pair_ecu` | No | Store current vehicle as paired (saves VIN + ECU info to NVS) |
| `remove_pairing` | **Yes** | Clear stored pairing data |
| `configure_logger` | No | Push variable configuration to patched ECU |
| `get_logger_data` | No | Get all current logger variable values (JSON object) |
| `get_single_variable` | No | Get one variable by name |
| `write_ecu` | **Yes** | Write data to ECU RAM address |

### System Commands

| Command | Auth Required | Description |
|---------|:---:|-------------|
| `unlock` | — | Authenticate with password (`sefi_admin_2024`) |
| `list_commands` | No | List all available commands |
| `get_errors` | No | Get error log history |
| `clear_errors` | **Yes** | Clear error log |

### Filesystem Commands

| Command | Auth Required | Description |
|---------|:---:|-------------|
| `fs_info` | No | Filesystem usage (total, used, free) |
| `fs_list` | No | Directory listing |
| `fs_read` | No | Read file (base64 encoded) |
| `fs_write` | No | Write file (base64 data) |
| `fs_delete` | No | Delete file or directory |
| `fs_mkdir` | No | Create directory |

### Example: Write ECU

```json
{
  "command": "write_ecu",
  "params": {
    "address": 1172142,
    "data": "4142434445464748",
    "format": "hex"
  }
}
```

### Example: Get Logger Data

```json
{"command": "get_logger_data"}
```

Response:
```json
{
  "command": "get_logger_data",
  "success": true,
  "message": "Command executed successfully",
  "data": {
    "nmot_w": 812.5,
    "tmot": 91.2,
    "wdkba": 23.4,
    "pvdg_w": 1456.3,
    "zwoutzyl_w": 12.4,
    "rl_w": 34.8,
    "frm_w": 0.98
  }
}
```

---

## 15. Quick-Reference Tables

### All CAN Messages Sent by Dongle

| When | TX Bytes | Purpose |
|------|----------|---------|
| Discovery | `3E 00` | Tester Present (find ECU) |
| Keep-alive | `3E 00` | Prevent ECU diagnostic timeout |
| Read VIN | `22 F1 90` | Get 17-char VIN |
| Read Boxcode | `22 F1 87` | Get ECU part number |
| Read SW Version | `22 F1 89` | Get software version string |
| Read Build ID | `22 F1 F4` | Get build identifier |
| Check Patch | `3E 39 01` | Test if ECU has logger patch |
| Get Patch Info | `3E 01 80` | Get buffer address and version |
| Configure Logger | Variable-length | Send variable list to ECU |
| Poll Logger | Variable-length | Request current variable values |
| Write RAM | `3E 39 01 [mid] [addr] [data]` | Write up to 64 bytes per chunk |
| Write Follow-up | `02 3E 37` | Retry after ECU "not ready" |

### All CAN Message IDs

| ID | Hex | Direction | Standard |
|----|-----|-----------|----------|
| ECU Request | `0x7E0` | Dongle → ECU | ISO 14229 physical |
| ECU Response | `0x7E8` | ECU → Dongle | ISO 14229 physical |
| Broadcast | `0x7DF` | Dongle → All | ISO 14229 functional |

### UDS Service Summary

| SID | Name | Subfunctions Used |
|-----|------|-------------------|
| `0x3E` | Tester Present | `0x00` (keepalive), `0x39` (live mode / write), `0x01 0x80` (patch info) |
| `0x22` | Read Data By ID | DIDs: `F190`, `F187`, `F189`, `F1F4` |

### ECU Connection Status Values

| Status | Meaning |
|--------|---------|
| `DISCONNECTED` | No ECU communication |
| `CONNECTING` | Discovery/identification in progress |
| `CONNECTED_UNPATCHED` | ECU found, stock firmware, keepalive only |
| `CONNECTED_PATCHED` | ECU found, SEFI patch present, logger active |
| `ERROR` | Communication failure |
