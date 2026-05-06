# Communication v1 — UDS Protocol (Sean / Albin Reference)

> **What this is:** Complete reverse-engineering of how the v1.0 SEFI dongle
> firmware (Sean's `firmware_sefi_v1`, with Albin's `ALBIN_WIP/sefi_firmware`
> as the readable C source) talks to the Audi RS7 MDG1 ECU over UDS.
>
> **Sources:** Two independent references that cross-validate each other:
> 1. The compiled v1.0 firmware on the dongle: `/Users/rabbit/Downloads/version 1.5/app0.bin` — strings, log format strings, embedded constants
> 2. Albin's source code: `~/esp/obd/SEFIv1/ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/src/` — readable C
> 3. Live serial captures from the dongle in-vehicle (2026-05-01 + 2026-05-03)
> 4. Existing reverse-engineering doc `~/esp/obd/SEFIv1/HW/OLD_FIRMWARE_REVERSE_ENGINEERING.md`
>
> **Reader: this is the spec we're cloning into FUTUNER v2.** Where the v1
> design used Sean's SCPN binary calibration files, the FUTUNER replacement
> uses small JSON map files (<256 KB) — the over-the-wire UDS protocol stays
> the same.

---

## 1. Physical and transport layer

| Parameter | Value | Source |
|---|---|---|
| Bus | CAN 2.0B, 11-bit identifiers | ALBIN can_driver.c |
| Baud | 500 kbps (fixed) | TWAI driver init |
| Transceiver | NCV7344D13R2G (on Sean's PCB) | HW/PROJECT_OVERVIEW.md |
| TX GPIO | **21** (on the v1.0 SEFI dongle) | binary-verified 2026-05-03, twai_general_config_t in DROM @ 0x6cc0 |
| RX GPIO | **14** (on the v1.0 SEFI dongle) | same |
| Driver | ESP-IDF TWAI v2 (`twai_driver_install_v2`) | strings in app0.bin |
| Filter | accept-all (no hardware filter) | strings in app0.bin |

### CAN IDs

| ID | Direction | Name |
|---|---|---|
| `0x7E0` | Dongle → ECU | Physical TX (all dongle requests) |
| `0x7E8` | ECU → Dongle | Physical RX (all ECU responses) |
| `0x7DF` | Dongle → All | Functional broadcast (defined, not used) |

### ISO-TP

Standard ISO 15765-2. Library: `isotp-c` (open source) with thin shims
in `src/can/isotp_shims.c`. Buffers: 4096 bytes each direction. Frame types
(SF/FF/CF/FC) are standard. Nothing custom here.

---

## 2. UDS service set (deliberately tiny)

Only **two** standard ISO 14229 services are used. Sean's "secret sauce" is
**custom sub-functions on TesterPresent (0x3E)**, made available by a patched
ECU firmware (see §6).

| Service | Use | Standard? |
|---|---|---|
| `0x3E` Tester Present | discovery, keep-alive, **and the patched-ECU live operations** | yes — but sub-functions are non-standard |
| `0x22` Read Data By Identifier | read VIN, boxcode, SW version, build ID | yes |

### Negative response codes (handled in `uds_get_nrc_name`)

| NRC | Name |
|-----|------|
| `0x10` | GeneralReject |
| `0x11` | ServiceNotSupported |
| `0x12` | SubFunctionNotSupported |
| `0x13` | IncorrectMessageLength |
| `0x22` | ConditionsNotCorrect |
| `0x31` | RequestOutOfRange |
| `0x33` | SecurityAccessDenied |
| `0x78` | ResponsePending |

Positive response = service ID + 0x40, e.g. positive response to `0x22` is
`0x62`, to `0x3E` is `0x7E`.

---

## 3. UDS request/response framing helpers

From ALBIN `state_machine/uds_protocol.[ch]`:

```c
typedef struct {
    uint8_t service;
    uint8_t data[255];
    uint16_t length;
} uds_request_t;

void uds_build_tester_present(uds_request_t *req,
                              uint8_t subfunction,
                              uint8_t parameter);
void uds_build_read_data_by_id(uds_request_t *req, uint16_t did);

bool uds_parse_response(const uint8_t *data, uint16_t length,
                        uds_response_t *resp);
bool uds_is_positive_response(const uds_response_t *resp,
                              uint8_t expected_service);
bool uds_extract_data(const uds_response_t *resp, uint16_t expected_did,
                      uint8_t *out_data, uint16_t *out_length,
                      uint16_t max_length);
```

The wire format for ReadDataByID is the standard `[0x22, did_hi, did_lo]`.
TesterPresent format is non-standard for the patch operations — see §6.

---

## 4. Connection state machine — full sequence

22 states total in v1, all run in the CAN task (`can_task` in main.c).
Documented from ALBIN `connection_manager.c` and verified against live serial
captures.

```
BOOT
 │
 ▼
 DISCOVERING ─── TX [0x3E, 0x00] ─── timeout (3 s) → retry
 │
 ▼  on positive 0x7E response
 REQUEST_VIN ─── TX [0x22, 0xF1, 0x90]
 │
 ▼  62 F1 90 [17 bytes ASCII VIN]
 CHECK_PAIRING
 │   ├─ NVS has same VIN? → SKIP to CHECK_PATCH_STATUS
 │   └─ no → REQUEST_SERIAL ... (full identification chain)
 │
 ▼
 REQUEST_SERIAL  ── 0x22 0xF1 0x87 → "4K0907557G" (boxcode)
 ▼
 REQUEST_SOFTWARE_VERSION  ── 0x22 0xF1 0x89 → "0003"
 ▼
 REQUEST_BUILD_ID          ── 0x22 0xF1 0x?? → "MDG1 CB.06.043.0 023.00"
 ▼
 REQUEST_EXTENDED_SESSION  ── 0x10 0x03 (Extended diag session)
 ▼
 CHECK_PATCH_STATUS        ── 0x3E with UDS_SUBFUNCTION_LIVE_MODE_ENABLE
 │   ├─ positive (0x7E …) → patched, live mode now on
 │   └─ negative (NRC 0x12 SubFunctionNotSupported) → unpatched ECU
 ▼
 REQUEST_PATCH_INFO        ── 0x3E [16-bit DID byte sequence]
 │   response: 0x7E 0x01 0x80 0x03 0xFF 0x00 0x8A 0x50 0x01 0xD2 0x93
 │   parses to: patch_version=V2, buffer_size=1023, log_buffer_addr=0x5001D293
 ▼
 CHECK_LOGGER_CONFIG → CONFIGURE_LOGGER → WAIT_LOGGER_CONFIG_RESPONSE
 ▼
 CONNECTED ⇄ POLLING_LOGGER (1 Hz default; faster modes available)
```

The `is_patched` flag determines what the dongle is allowed to do:

| Flag value | What's allowed |
|---|---|
| `is_patched = false` | Read DIDs (VIN, boxcode, SW version) only. No logger, no RAM write. State = CONNECTED_UNPATCHED. |
| `is_patched = true` | Logger, RAM write, full live mode. State = CONNECTED_PATCHED. |

---

## 5. ECU identification DIDs (standard 0x22 reads)

| DID | Name | Format | Example |
|---|---|---|---|
| `0xF190` | VIN | 17-byte ASCII | `WUAPCBF28NN902533` |
| `0xF187` | Boxcode / Serial | up to 11-byte ASCII | `4K0907557G ` (10 chars + pad) |
| `0xF189` | Software Version | 4-byte ASCII | `0003` |
| `0xF1??` | Build ID | up to 24-byte ASCII | `MDG1  CB.06.043.0 023.00     ` |
| `0xF1AD` | (likely) Hardware Version | similar | (cached `4K0907557G`) |

ReadDataByID wire shape:
```
TX:  22 <didhi> <didlo>
RX:  62 <didhi> <didlo> <data...>
```

---

## 6. The "patch" — Sean's custom ECU firmware mod

This is the central trick the dongle relies on. An **unpatched** stock MDG1
ECU will not respond positively to anything in this section — these are
sub-functions added to the ECU's UDS handler by a custom firmware patch
(loaded via standard ECU flashing tools, separately from the dongle).

### 6.1 Patch detection (LIVE mode enable)

Dongle sends:
```
TX:  3E <UDS_SUBFUNCTION_LIVE_MODE_ENABLE> 0x01
```
- `UDS_SUBFUNCTION_LIVE_MODE_ENABLE` is a magic byte known to Sean's patch.
  Looking at the binary captures, it appears to be a high bit-set value like
  `0x80`-something (exact value unconfirmed in source — defined as macro in a
  header that wasn't recovered. The runtime call uses the macro so we can
  determine it by capturing live CAN traffic with the candleLight sniffer.)
- Patched ECU returns: `7E ...` (positive ACK)
- Unpatched ECU returns: `7F 3E 12` (NRC SubFunctionNotSupported)

### 6.2 Patch info request

After live mode is on, dongle queries patch metadata:
```
TX:  3E <UDS_PATCH_INFO_REQUEST_HI> <UDS_PATCH_INFO_REQUEST_LO>
RX:  7E 01 80 03 FF 00 8A 50 01 D2 93
     │  │  │  │  │  │  │  └─────────┴──── log buffer ECU address (32-bit BE)
     │  │  │  │  │  │  └────────────────── 0x8A (struct version marker)
     │  │  │  │  └──┴───────────────────── 0xFF 0x00 (separator / flags)
     │  │  └──┴──────────────────────────── 0x80 0x03 = buffer size (1023, BE)
     │  └────────────────────────────────── 0x01 = patch_version (V2 = 0x01)
     └───────────────────────────────────── 0x7E (positive 0x3E response)
```

For our RS7 dongle the parsed values are:
- `patch_version` = V2 (`0x01`)
- `patch_buffer_size` = `1023` bytes
- `log_buffer_address` = `0x5001D293` (in ECU's RAM)

The 1023-byte buffer at `0x5001D293` is the workspace the dongle uses for
logger configuration AND data return.

### 6.3 Logger configure (sub-function 0x32)

```
TX:  3E 32 <bp_h> <bp_m> <bp_lh> <bp_ll> <total_size> <group_count> [groups...]
RX:  7E 32 [success]
```

Where `bp` = log buffer pointer (i.e. `0x5001D293`), and each group is a
contiguous run of variables to read in one ISO-TP transfer. From a real
example captured on the dongle:

```
3E 32 50 01 D6 69 00 28 04 \
       50 01 02 BA 86 02 C9 A6 02 CB 0C 02 CD 84 \   group 1
       04 60 01 02 52 2A 02 56 60 \                  group 2
       01 B8 42 01 BF 38 02 60 02 02 06 18 02 06 F8 \ group 3
       00
```
- `0x50 0x01 0xD6 0x69` = pointer 0x5001D669 (where in patch buffer)
- `0x00 0x28` = total response size (40 bytes)
- `0x04` = group count (4 — but observed 3 groups; field may include
  inverse-segment count or be padded)
- Each group prefix: `<addr_high_byte> <addr_mid_byte>` then list of
  `<size_byte> <addr_low_h> <addr_low_l>` triples

Variables are bucketed into "groups" sharing an upper-16-bit address range,
so the ECU can do one continuous DMA read per group.

### 6.4 Logger poll (sub-function 0x33)

```
TX:  3E 33
RX:  7E 33 [40 bytes of variable values, packed per logger config]
```

Typical poll cycle: ~50 ms request→response. Dongle waits, parses, calls
`logger_config_parse_poll_response()` which decodes each variable using
its scale + offset from `logger_variables.c`.

### 6.5 ECU RAM write (sub-function 0x39)

This is what makes flex-fuel real-time tuning possible.

```
TX:  3E 39 01 <mid> <addr_h> <addr_m> <addr_l> <data_byte_0> ... <data_byte_N>
RX:  7E 39 01 <mid> <addr_h> <addr_m> <addr_l> [optional ack]
```
- Sub-function `0x39` (`UDS_WRITE_RAM_SUBFUNCTION`)
- Fixed byte `0x01` (`UDS_WRITE_FIXED_BYTE`)
- `mid_byte` is **boxcode-specific**:
  - `4K0907557G__0003` (RS7 C8) → `0x80`
  - `8W0907559H__0008` → `0x09`
  - others per `boxcode_database.json`
- `<addr_h> <addr_m> <addr_l>` = 24-bit ECU memory address (NOT the full 32-bit; the high byte is implicit / fixed). The full target = `addr - address_offset` where `address_offset` is also boxcode-specific (`0x000000` for RS7 → no shift).
- Max chunk = 64 bytes per write (`UDS_WRITE_MAX_CHUNK_SIZE`). The dongle automatically chunks larger payloads.
- After each chunk the dongle waits for ACK before sending the next. There is also a `UDS_WRITE_FOLLOWUP_REQUEST_SIZE` of 3 bytes used as a write-completion handshake (TBD — read the rest of `ecu_write.c` if needed).

The patched ECU's job on receiving `[0x3E, 0x39, 0x01, mid, addr*3, data*N]`:
1. Validate `mid` matches its boxcode signature.
2. Compute `address = supplied_addr + offset_from_patch`.
3. Write `data` to that RAM address.
4. ACK with `0x7E 0x39 0x01 ...`.

This is how the dongle changes fuel/spark/boost maps in RAM without
reflashing the ECU. The ECU keeps using the modified maps until power-cycle
(then the "live" patches are gone — they live in volatile RAM only).

---

## 7. Boxcode-driven configuration

Each ECU boxcode has a config table loaded from `boxcode_database.json`. The
dongle uses these per-boxcode values throughout the protocol:

| Field | Used for | Example (RS7 4K0907557G__0003) |
|---|---|---|
| `is_big_endian` | byte order when reading variables | false (LE) |
| `write_mid_byte` | the `mid` byte in 0x39 RAM writes | `0x80` |
| `write_address_offset` | subtracted from target before sending | `0x000000` |
| `ethanol_memory_address` | where ECU stores ethanol % | `0x11E6AE` |
| `speed_display_memory_address` | where to write cluster speed override | `0x09F93E` |
| `variables[]` | logger variable definitions (name, addr, size, scale, offset) | 23-53 entries |

`logger_variables.c` is the embedded copy of this table for each supported
boxcode. The cloud server in v1 (`api.dynoscorpion.com`) was the source of
truth — dongle could pull updated tables.

---

## 8. The flex-fuel real-time loop

The unique value prop of this system: as ethanol % changes (e85 sloshes
during fueling, or you stage E85 from a sub-tank), the dongle modifies the
ECU's fueling/timing maps in real time so the engine stays optimally tuned
across the blend range.

### Data flow

```
   BLE ethanol sensor (SEFI_P @ 64:e8:33:b6:bc:02)
        │ NimBLE notifications
        ▼
   ETHANOL manager (ethanol_manager.c)
        │ float ethanol_pct (e.g. 62.17)
        ▼
   Logger poll loop reports ECU's current ethanol register value
        │ uint16_t raw_ecu_eth (e.g. 0x8000)
        ▼
   CAL state machine (calibration_manager.c)
        │ Decides: ethanol changed → re-blend maps → write new values
        ▼
   For each "blendable" map in active SBF:
        - Read current map values (from NVS-cached SBF)
        - Compute blend = blend_map(ethanol_pct) ∈ [0..1]
        - new_value = base_value*(1-blend) + e85_value*blend
        - Push to ECU via UDS 0x3E 0x39 (chunked)
```

### Ethanol validation gate (the v1 DRM)

Before touching the ECU, dongle validates that the SBF is "for this ECU":

1. SBF header has `ethanol_bit_count` (e.g. 10) and `ethanol_random` (e.g. 0x2AF).
2. Dongle reads ECU ethanol register (via logger poll).
3. Computes `extracted = raw_ecu_eth & ((1 << bit_count) - 1)`.
4. Compares to `ethanol_random`. If not equal → reject the SBF.

The ethanol register in MDG1 is 16-bit. Sean's server-side **SBF patcher**
writes the per-VIN `ethanol_random` value into the ECU's ethanol register
during initial pairing (so for that one ECU, the low bits of the ethanol
reading match the SBF). When you upload an SBF Sean signed for VIN A and
try to use it on VIN B, validation fails because B's ethanol register has
B's `ethanol_random`, not A's.

This is the source of the "Ethanol validation FAILED" loop we saw on the
broken dongle — the ECU's `ethanol_random` had been overwritten or never
set.

### What FUTUNER (v2) does instead

Replace SBF (binary, ~35 KB) with **JSON map files (<256 KB)** that:
- Encode the same map data in a human-readable form (axis arrays + 2-D Z-array per map)
- Encode the same per-VIN validation field (now openly documented, not DRM)
- Are compiled/served by SRM's own cloud (`api.sillyrabbitmotorsport.com`)

The on-the-wire UDS protocol stays identical (services, sub-functions,
chunking, mid_byte). Only the on-device file format changes.

---

## 9. Logger variable definition format

```c
typedef struct {
    const char *name;          // "nmot_w"
    const char *display_name;  // "Engine Speed"
    const char *unit;          // "rpm"
    uint32_t address;          // ECU memory address (e.g. 0x60020618)
    uint8_t  size;             // bytes (1, 2, 4)
    float    scale;            // multiplied with raw value (0.25 for RPM)
    float    offset;           // added after scale
    bool     is_required;      // pulled even if not in user profile
    bool     is_signed;        // sign-extend before scale
} logger_variable_def_t;
```

Each boxcode has a `boxcode_config_t` aggregating its variables plus the
write metadata above. Up to 32 variables can be in the active logger
config simultaneously (limited by patch buffer size of 1023 bytes).

---

## 10. Security model (what little there is)

There is **no UDS-level security access** (`0x27 SecurityAccess` is not used).
All "security" is application-layer:

- The patched ECU only ACKs the custom sub-functions if its firmware was
  patched — that's the gate. Unpatched ECUs reject those sub-functions.
- The cloud server gates which dongles can download which SBFs (Bearer auth).
- The SBF ethanol-validation locks each tune to one ECU's `ethanol_random`.
- The dongle's web UI requires a WebSocket admin password
  (`futuner_admin_2024` per memory) for write commands.

A determined attacker with a patched ECU and a custom dongle can do anything
this protocol can do. The protections are commercial / IP, not cryptographic.

---

## 11. Minimum proof-of-concept implementation checklist

If you (FUTUNER v2) want to talk to the same ECU using the same protocol:

1. CAN driver at GPIO 21/14 (BOARD_V10) or your PCB's pins, 500 kbps, ISO-TP.
2. Send `[0x3E, 0x00]` until `0x7E` arrives → connected.
3. Read DIDs `0xF190 / 0xF187 / 0xF189` → identify ECU.
4. Send `[0x10, 0x03]` → extended diagnostic session.
5. Send `[0x3E, <LIVE_MODE_BYTE>, 0x01]` — if positive, ECU is patched.
6. Send `[0x3E, <PATCH_INFO_DIDs>]` → parse `[ver, ?, ?, sz_h, sz_l, ...addr]`.
7. Build logger config message (sub-function 0x32) with up to 32 variables
   bucketed into address-range groups, each group = `[size, addr_h, addr_l]`.
8. Poll with `[0x3E, 0x33]` at desired Hz.
9. Write to ECU RAM with `[0x3E, 0x39, 0x01, mid, addr*3, data*N]`.
10. Honor the 64-byte chunk limit for writes; sequence chunks with
    `WAIT_FOLLOWUP` between them for safety.

---

## 12. Open questions / things to confirm with live capture

- Exact byte value of `UDS_SUBFUNCTION_LIVE_MODE_ENABLE` (Albin's source uses
  it as a macro; the macro definition wasn't in the recovered files we
  inspected). Capture with the candleLight sniffer to determine.
- Exact bytes of `UDS_PATCH_INFO_REQUEST` (the 2-byte DID-shaped sequence).
- The "follow-up" 3-byte handshake after a chunk write — is it always sent
  or only on the last chunk? Need to step through `ecu_write.c` end-to-end.
- The `0x04` count byte in the captured logger config — does it represent
  group_count, total_groups+inverse_count, or something else?

These can all be answered by tcpdump-on-CAN: run candleLight sniffer in
parallel with the dongle and capture a full session. Tools for this are
already in `~/esp/obd/SEFIv1/can_sniff.py`.

---

## 13. References on disk

| Path | What |
|---|---|
| `~/esp/obd/SEFIv1/HW/OLD_FIRMWARE_REVERSE_ENGINEERING.md` | Pre-existing 715-line RE doc (binary string analysis) |
| `~/esp/obd/SEFIv1/ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/src/state_machine/uds_protocol.c` | UDS request/response framing |
| `~/esp/obd/SEFIv1/ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/src/state_machine/connection_manager.c` | Full state machine |
| `~/esp/obd/SEFIv1/ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/src/ecu_write/ecu_write.c` | RAM write protocol |
| `~/esp/obd/SEFIv1/ALBIN/ALBIN_WIP/ALBIN_WIP/sefi_firmware/src/logger/logger_config.c` | Logger config + poll msg builder |
| `/Users/rabbit/Downloads/version 1.5/app0.bin` | Compiled v1.0 firmware (active build) |
| `/Users/rabbit/Downloads/version 1.5/extracted/strings/log_format.txt` | Log format strings (1167 entries — narrative of every event) |
| `/Users/rabbit/Downloads/version 1.5/extracted/fs/cal/stage1_patched.sbf` | Active calibration (Sean's SCPN binary) |
| `/Users/rabbit/Downloads/stage1.sbf` | Identical-size copy in your Downloads (you already had it) |
| `~/esp/obd/SEFIv1/HW/4K0907557G__0003_FULL.xdf` | TunerPro XDF — map definitions for this ECU (the source of truth for FUTUNER's JSON map files) |
| `~/esp/obd/SEFIv1/boxcode_database.json` | Per-boxcode write mid/offset/ethanol addresses |

---

*Generated 2026-05-03. Authored by reverse-engineering Sean's compiled
firmware against Albin's source code copy. Cross-validated with live serial
captures of the dongle in the Audi RS7. Use this as the reference spec while
building FUTUNER v2's UDS protocol layer — replicate the wire protocol
exactly; replace the SCPN map file with JSON.*
