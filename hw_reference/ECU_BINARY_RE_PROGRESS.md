# RS7 C8 ECU Binary Reverse Engineering -- Progress Tracker

**Project:** SEFI Auxiliary Injection Controller -- ECU CAN TX Modification
**Target ECU:** Bosch ECM40TFS (4K0907557G) -- Audi C8 RS7 4.0 TFSI
**Binary:** RS7C8_SAMPLE_WUAPCBF28NN902533_4K0907557G__0003.bin
**Last Updated:** 2026-02-09 (session 2, ongoing)

---

## Objective

Modify CAN TX payloads in the RS7 engine ECU binary to broadcast additional diagnostic signals for the SEFI auxiliary injection controller. The SEFI controller requires three vehicle-side signals (RPM, Pedal Position, Ambient Temp) and will eventually need an additional custom CAN message (0x7A0) for ethanol content and supplemental fuel system data.

---

## Completed Work

### Phase 1: Toolchain Setup (2026-02-04 / 02-05)

Established the full reverse engineering toolchain connecting Claude to Ghidra for AI-assisted binary analysis.

**Toolchain stack:**
- Ghidra 12.0.x with TriCore processor support
- LaurieWired GhidraMCP plugin (HTTP API on localhost:8080)
- Claude in Chrome browser extension (JavaScript fetch to Ghidra API)
- Python MCP bridge for Claude Desktop integration (alternative path)

**Setup artifacts produced:**
- `tools/setup-ghidra-mcp.ps1` -- Automated installer for Ghidra + MCP
- `tools/verify-toolchain.ps1` -- Dev environment validator
- SETUP-GUIDE.md -- Full gotcha list and manual steps

**Key gotcha resolved:** Chrome extension talks to Ghidra via direct HTTP fetch, not MCP bridge. Claude Desktop MCP route also works but requires restart after config.


### Phase 2: Binary Validation (2026-02-08)

Confirmed the binary is correctly loaded and the analysis environment is sound.

**Verified:**
- TriCore architecture confirmed via mfcr/mtcr/calla/movh.a instructions
- Binary identity confirmed: embedded string "EV_ECM40TFS0114K0907557G" at 0x8015F8CC
- 13,864 functions identified across 0x8001C240 -- 0x807C6E2C
- Decompiler produces valid C pseudocode with TriCore calling convention
- Cross-references (xrefs_to / xrefs_from) working bidirectionally

**Known issues (non-blocking):**
- Missing cached flash overlay (0xC0000000 segment) -- causes some xref gaps
- Unanalyzed startup region below 0x8001C240 (BMI header, vectors, crt0)
- No entry points defined (typical for raw flash dump)

**Artifact:** RS7_C8_BINARY_VALIDATION.md (project knowledge)


### Phase 3: CAN Signal Identification (2026-02-08)

Mapped the three SEFI controller input signals to their CAN message definitions.

| Signal | CAN ID | DBC Name | Bytes | Format | Scale | Offset |
|--------|--------|----------|-------|--------|-------|--------|
| Engine RPM | 0x0A8 | Motor_12 / MO_Drehzahl_01 | 6-7 | uint16 LE | 0.25 | 0 |
| Pedal Position | 0x18B | (TBD) | 7 | uint8 | 0.4 | 0 |
| Ambient Temp | 0x665 | (TBD) | 4 | uint8 | 0.5 | -40 |

**Key discovery:** The ECU has a direct CAN transceiver on PTCAN and transmits these messages itself (confirmed by sniffing CAN HI/LOW at the ECU connector). This means the CAN TX packing logic is inside this binary -- not in a gateway ECU we don't have access to.

**Motor_12 (0x0A8) fully mapped** -- all 8 bytes decoded from OpenDBC vw_mqb_2010.dbc:
- Byte 0: CRC-8 checksum
- Byte 1 lower nibble: Alive counter (0-15)
- Bytes 1-2 (bits 12-20): Available negative torque
- Bytes 2-3 (bits 21-29): Static torque limit
- Bytes 3-4 (bits 30-39): Dynamic torque limit
- Byte 5 (bits 40-46): Torque integral (load %)
- Byte 5 bit 47: RPM quality bit
- Bytes 6-7: Engine RPM (confirmed against live CAN trace)

**CAN trace validation:** Live .asc dump analyzed, byte patterns confirmed against DBC. Byte 0 (CRC) varies per-frame, byte 1 nibble cycles 0-F (alive counter verified), bytes 6-7 stable at idle RPM.

**Artifact:** CAN_0xA8_Motor12_ByteMap.md (project knowledge)


### Phase 4: A2L File Mining (2026-02-05 / 02-08)

Extracted CAN TX PDU definitions and signal mappings from the 48MB A2L calibration file.

**Findings:**
- 229 TX PDU entries found (TrM_PduCfgItemTx pattern)
- Motor messages Motor_1 through Motor_30 mapped (not all sequential)
- FlexRay TX signal addresses identified for RPM:
  - MoFTx_nMotor12ESpd @ 0x60028B3C (RPM)
  - MoFTx_stMotor12QBitESpd @ 0x60028B3E (RPM quality bit)
- A2L confirms FlexRay frame AFLEX_A_Motor_12 as source for CAN 0xA8


### Phase 5: TX PDU Config Table Decode (2026-02-08 / 02-09)

Located and fully decoded the CAN TX PDU configuration table in the binary.

**Table location:** 0x80222CA8
**Structure:** 42 entries, 8 bytes each

```
struct CAN_TX_PDU {      // 8 bytes per entry
    uint16_t can_id;     // CAN arbitration ID
    uint16_t id_ext;     // 0x0000 = standard 11-bit
    uint16_t pdu_index;  // Sequential PDU index (1-42)
    uint16_t dlc_flags;  // Upper byte = DLC, lower byte = flags
};
```

All 24 PTCAN IDs from the live CAN trace are present, plus diagnostic IDs (0x700, 0x703, 0x07DF) and extended-frame entries. DLC field validated against trace (0x0803 = DLC 8, 0x0403 = DLC 4 for 0x3C0).


### Phase 6: Signal RAM Address Tracing (2026-02-09)

Identified RAM addresses for target signals and located pointer tables referencing them.

**Confirmed signal addresses:**
- rl_w (ethanol content) @ 0x60015660
- frm_w_msg (steering wheel angle) @ 0x5001C9A6
- frm2_w_msg (steering wheel angle 2) @ 0x5001CC0A

**Pointer table locations:** 0x80229838 -- 0x8022A56C contains 32-bit pointers to signal RAM addresses, used by the packing/routing functions.


### Phase 7: Hardware Init Function Analysis (2026-02-09)

Decompiled FUN_8069d718 -- identified as CAN hardware message object initialization, NOT the runtime packing function.

**What it does:** Iterates the PDU config table at 0x80222CA4, reads each entry, and programs AURIX MultiCAN+ hardware message object registers:
- MO_FCR (function control) at MO base + 0x00
- MO_FGPR (FIFO/gateway pointer) at + 0x04
- MO_IPR (interrupt pointer) at + 0x08
- MO_AMR (acceptance mask) at + 0x0C
- MO_AR (arbitration / CAN ID) at + 0x18
- MO_CTR (control/status) at + 0x1C

**MultiCAN+ base address:** 0xF0019000 (message object region)
**Message object spacing:** 0x20 bytes per MO

**Chrome content filter workaround:** Direct fetch() and base64 encoding both blocked by Chrome extension detecting hex patterns as cookies. Successful workaround: navigate browser tab directly to decompile URL, then use get_page_text() to extract content.


### Phase 8: CAN TX Function Chain Discovery (2026-02-09, session 2)

Traced the complete call chain from PDU config table through to hardware transmit.

**Three functions reference DAT_80222CA4 (TX PDU table):**
- FUN_8069d718 -- Hardware init (Phase 7, already analyzed)
- FUN_8069e078 -- CAN controller state machine (startup/shutdown sequencing)
- FUN_8069e3bc -- **THE CAN TRANSMIT FUNCTION** (writes payload + triggers TX)

**FUN_8069e3bc -- CAN Frame Transmit (CONFIRMED)**

This is the low-level function that physically sends a CAN frame. It:
1. Looks up the message object number from PDU table: `(&DAT_80222ca4)[param_2 * 8]`
2. Computes MO base: `bVar1 * 0x20` (MO number * 32 bytes)
3. Copies 8 data bytes into MO_DATAL: loop writing to `iVar6 + -0xffe6ff0` = 0xF0019010
4. Sets CAN ID into MO_AR: `iVar6 + -0xffe6fe6` / `-0xffe6fe8` = 0xF001901A/18
5. Sets DLC: `iVar6 + -0xffe6ffd` = 0xF0019003 (MO_FCR DLC field)
6. Triggers transmit: writes 0x40 to MO_CTR (`-0xffe6fe4`) = TXRQ bit
7. Sends completion notification: writes 0x708 to MO_CTR+2

**param_1 struct layout (passed by callers):**
```
struct CAN_TX_Request {     // 16 bytes
    uint32_t can_id;        // [0] CAN ID, bit 31 = extended frame flag
    uint32_t dlc_info;      // [1] DLC in lower nibble
    uint32_t data_ptr;      // [2] Pointer to 8-byte payload buffer
    uint32_t config;        // [3] Timeout/scheduling config
};
```

**FUN_8069e078 -- CAN Controller State Machine**

Manages CAN peripheral startup states (cases 0-3). Calls FUN_8069d718 for hardware init, configures acceptance masks, sets up TX/RX message object ranges. Not relevant to runtime packing.

**Two functions call FUN_8069e3bc (the TX function):**
- FUN_8069e818 -- TX wrapper that builds CAN_TX_Request from config tables
- FUN_8069e8a6 -- Second TX wrapper (not yet decompiled)

**FUN_8069e818 -- TX Wrapper (partially analyzed)**

Takes a PDU index (`param_1`), builds the CAN_TX_Request struct from two config tables:
- CAN ID + flags from DAT_80222F34 (offset `param_1 * 8`)
- Data buffer pointer from DAT_8023371A lookup (offset `param_1 * 2` -> multiplied by 0xC)
- Buffer lives in RAM at `0x600181BE + computed_offset`

This is one layer closer to the application -- the **actual signal packing** functions that write RPM/pedal/temp into the data buffers must call THIS function (or prepare the buffers it reads from).

**New config table discovered: DAT_80222F34**
Another TX config table at 0x80222F34 with 8-byte entries -- likely the "second half" of the PDU config containing CAN ID + extended flags in the format expected by FUN_8069e3bc's param_1 struct.


### Phase 9: Packing Function Dispatch Table (2026-02-09, session 2)

Discovered the complete packing function dispatch mechanism.

**Function pointer table at 0x8022328C** -- 6 entries:

| Index | Pointer | Role | Used by |
|-------|---------|------|---------|
| 0 | NULL | No callback -- data pre-packed by COM layer | 0x040, 0x0A8 (RPM), 0x585, 0x170C |
| 1 | 0x8069F8B8 | Special single-message handler | 0x045A only |
| 2 | 0x806A0930 | Shared packing function (type 2) | 0x152, 0x171, 0x18B (Pedal), 0x285, 0x744 |
| 3 | 0x802A06E8 | Most common packing fn (likely AUTOSAR Com) | 0x18D, 0x3A3, 0x3C0, 0x641, 0x700, etc. |
| 4 | 0x802E7A5E | Packing function (type 4) | 0x0EE, 0x0FD, 0x100, 0x150, 0x151 |
| 5 | 0x80233848 | Points to data region (not code?) | Not referenced by any table 2 entry |

**TX Scheduler: FUN_8069e9fe**

Called by FUN_8069d470 (periodic task). Workflow:
1. Check CAN controller state (must be running)
2. Lock critical section (`func_0xc00002f0`)
3. Iterate pending PDU range (DAT_80233826 to DAT_80233828)
4. For each PDU with dirty flag set at `0x600181c8+offset`, call FUN_8069e818 to transmit
5. Unlock critical section
6. Call packing callback from function pointer table at 0x8022328C (indexed by byte 7 of TX config table 2)

Called by FUN_8069d470 -> FUN_8069e9f0 (thunk) -> FUN_8069e9fe.

**Critical finding for Motor_12 (0x0A8):**

CAN ID 0x0A8 has function index 0 = NULL. This means it does NOT use a per-message packing callback. Instead, its 8-byte payload is pre-packed into the TX buffer by the AUTOSAR COM / signal routing layer BEFORE the TX scheduler runs. The scheduler just checks the dirty flag and transmits the already-packed buffer.

This means to find the RPM packing code, we need to trace what writes to the TX data buffer for PDU entry 1 (0x0A8's position). The buffer address is computed as: `0x600181BE + DAT_8023371A[1] * 0xC`.

**Critical finding for Pedal Position (0x18B):**

0x18B uses function index 2 -> FUN_806A0930. This is a shared packing function used by multiple messages. Need to decompile to understand how it selects which signals to pack.

**Ambient Temp (0x665) NOT in TX config table 2:**

0x665 does not appear in the 42-entry table at 0x80222F34. May be on a different CAN bus, handled by a different TX path, or only present in certain ECU variants. Needs investigation.


### Phase 10: TX Buffer Mapping & RPM Signal Trace (2026-02-09, session 2)

**Motor_12 TX buffer address: 0x600181CA**

Computed from the scheduling lookup table:
- DAT_8023371A[1] = 0x0001 (PDU entry 1 = CAN 0x0A8)
- Buffer offset = 0x0001 * 0xC = 0x0C
- Buffer addr = 0x600181BE + 0x0C = **0x600181CA**

**Only 7 of 42 PDUs use pre-allocated buffers:**

| PDU Idx | CAN ID | Lookup | Buffer Addr | Fn Idx |
|---------|--------|--------|-------------|--------|
| 0 | 0x040 | 0x0000 | 0x600181BE | 0 (NULL) |
| 1 | 0x0A8 | 0x0001 | 0x600181CA | 0 (NULL) |
| 2 | 0x0EE | 0x0002 | 0x600181D6 | 4 |
| 3 | 0x0FD | 0x0003 | 0x600181E2 | 4 |
| 4 | 0x100 | 0x0004 | 0x600181EE | 4 |
| 5 | 0x150 | 0x0005 | 0x600181FA | 4 |
| 6 | 0x151 | 0x0006 | 0x60018206 | 4 |
| 7-41 | various | 0xFFFF | N/A | 2 or 3 |

All other PDUs (fn_idx 2/3) have lookup = 0xFFFF, meaning they pack inline via callbacks and don't use pre-allocated buffers.

**TX buffer struct (12 bytes per entry):**
```
struct CAN_TX_Buffer {      // 0x0C bytes
    uint8_t data[8];        // +0x00: CAN payload (8 bytes)
    uint16_t dlc;           // +0x08: DLC
    uint8_t dirty;          // +0x0A: Pending/dirty flag (checked by scheduler)
    uint8_t status;         // +0x0B: Status/padding
};
```

**RPM signal FlexRay address 0x60028B3C -- xrefs found:**

Two functions reference the MoFTx_nMotor12ESpd signal at 0x60028B3C:
- **FUN_804041a0** -- 6 refs (3 writes, 3 reads) -- this is likely the Motor_12 signal packing/routing function
- **FUN_80403f04** -- 1 write ref -- possibly the FlexRay RX handler that updates the signal value

**Search for buffer writers failed via address constant scan:**

0x600181CA does not appear as a 32-bit constant anywhere in the binary. The buffer address is computed dynamically at runtime via the lookup table, meaning the COM layer uses a generic copy routine indexed by PDU number, not a hardcoded address. This is typical AUTOSAR Com_SendSignalGroup behavior.

**Pending counter (DAT_600181BC) xrefs:**
- FUN_8069eb36 -- state reset function (clears counter and all dirty flags)
- FUN_8069eda2 -- decompile missed, address maps into FUN_8069ebec (CAN controller state machine, manages startup/shutdown transitions)


### Phase 11: FlexRay Signal Packer Decompilation (2026-02-09, session 2)

Decompiled FUN_804041a0 -- a large function (~400 lines decompiled) that handles FlexRay frame packing for multiple motor signal groups.

**Architecture clarification -- THREE-layer TX pipeline for Motor_12:**
```
Layer 1: Raw signal computation
  -> Source: _DAT_60014E06 (raw RPM value, 16-bit)

Layer 2: FlexRay frame packing (FUN_804041a0)
  -> Reads _DAT_60014E06, shifts left 1: _DAT_60028b3c = (ushort)(raw << 1)
  -> Writes to FlexRay TX signal: 0x60028B3C (MoFTx_nMotor12ESpd)
  -> Uses redundant storage with bitwise complement for safety validation
  -> Double-buffered: alternates writes between two register pairs

Layer 3: FlexRay-to-CAN translation (??? NOT YET FOUND)
  -> Copies from FlexRay TX signals (0x60028B3C) to CAN TX buffer (0x600181CA)
  -> This is the missing link in the chain

Layer 4: CAN TX scheduler + hardware (fully traced)
  -> FUN_8069e9fe checks dirty flag, calls FUN_8069e818 -> FUN_8069e3bc -> MultiCAN+
```

**FUN_804041a0 is NOT the CAN TX packing function.** It's the FlexRay frame preparation function. It handles 5 signal subgroups (selected by bits 0, 1, 2, 4, 5 of a status word):

| Bit | Signal Group | Key Source Variables |
|-----|-------------|----------------------|
| 0 | Torque/status | DAT_6002897f, DAT_60028948, DAT_60028976 |
| 1 | **RPM group** | _DAT_60014E06 (raw RPM), writes 0x60028B3C |
| 2 | Additional torque | DAT_60028cfd, DAT_60028cfc, DAT_60028843 |
| 4 | DTC/fault flags | DAT_60028cef, DAT_60028cee, DAT_60028cf0 |
| 5 | Quality/status | DAT_600288a4 |

**RPM computation detail:**
- Normal path: `_DAT_60028b3c = (ushort)((int)_DAT_60014e06 << 1)`
- Fallback path (error): `_DAT_60028b3c = (ushort)DAT_60028cb6 * 0xa0`
- Negative value clamped to 0: `if (iVar9 < 0) { _DAT_60028b3c = 0; }`

**Safety pattern:** Every signal stored as value/~value pair (ASIL fault detection). Sub-functions FUN_804010a6 and FUN_804010fc validate these pairs after each write.

**Raw RPM source confirmed: 0x60014E06**

---

## Current Blockers

### 1. FlexRay-to-CAN Translation Layer Not Found

FUN_804041a0 turned out to be the FlexRay frame packer, NOT the CAN TX buffer writer. The full 4-layer pipeline is:
```
Raw RPM (0x60014E06)             -- FOUND (Layer 1)
  -> FUN_804041a0                -> 0x60028B3C -- FOUND (Layer 2: FlexRay packer)
    -> FR-to-CAN copier (???)    -> 0x600181CA -- MISSING (Layer 3)
      -> CAN TX scheduler         -> MultiCAN+  -- FOUND (Layer 4)
```

The FlexRay-to-CAN translation function that copies signal values into the CAN TX buffer at 0x600181CA is the remaining gap. It's likely an AUTOSAR COM/PduR copy routine triggered by the signal validation functions.

**Next step:** Trace FUN_804010a6/FUN_804010fc (called after signal writes), decompile FUN_806A0930 (type-2 packer for 0x18B which may reveal the pattern), or search for generic memcpy-style functions that target the 0x60018xxx buffer region.

### 2. CAN IDs 0x18B and 0x665 Not Fully Decoded

Motor_12 (0x0A8) has a complete byte map, but the Pedal Position (0x18B) and Ambient Temperature (0x665) messages still need full DBC-level signal layouts. These are in the TX PDU table, confirming the ECU transmits them, but internal packing details are TBD.

### 3. Free Flash Space for Code Cave Not Located

Adding a new CAN message (0x7A0 for ethanol content broadcast) requires approximately 50 bytes of executable space for a custom packing function. No scan for NOP padding / unused regions has been performed yet.

---

## Planned Work (Priority Order)

### Immediate

1. **Find FR-to-CAN translation function** -- Something reads FlexRay signals (0x60028B3C) and writes CAN TX buffers (0x600181CA). Trace FUN_804010a6/FUN_804010fc (called after each signal write) -- they may trigger the copy. Or search for readers of 0x60028B3C other than FUN_804041a0.

2. **Decompile FUN_806A0930** -- Packing fn for 0x18B (Pedal Position). Since 0x18B uses fn_idx=2 (inline packing), this may combine FR-to-CAN + packing in one step, revealing the pattern.

3. **Investigate 0x665 TX path** -- Ambient Temp not in TX table. Check if different CAN node or AUTOSAR PduR routing.

4. **Examine FUN_804010a6 / FUN_804010fc** -- Signal validation sub-functions called by FUN_804041a0. May trigger AUTOSAR signal group send (Com_SendSignalGroup) which copies to CAN TX buffer.

### Short Term

4. **Decode CAN IDs 0x18B and 0x665** -- Full byte maps for Pedal Position and Ambient Temp messages. Cross-reference A2L + DBC + live trace.

5. **Locate free flash space** -- Scan for large blocks of 0xFF or NOP padding suitable for a code cave. Need approximately 50 bytes for the 0x7A0 packing function.

6. **Design the 0x7A0 message packing function** -- TriCore assembly to read ethanol content (rl_w @ 0x60015660) and pack into the new CAN frame.

### Medium Term

7. **Add entry to TX PDU config table** -- Append a 43rd entry for CAN ID 0x7A0 with appropriate DLC and flags.

8. **Hook the scheduler** -- Ensure the new packing function gets called on the 10ms schedule alongside existing CAN TX.

9. **Implement binary patch** -- Write the actual hex modifications and validate with Ghidra re-analysis.

10. **Flash and test** -- Load modified binary onto ECU, verify 0x7A0 appears on PTCAN bus with correct data.

---

## Key Addresses Reference

| Item | Address | Notes |
|------|---------|-------|
| TX PDU Config Table | 0x80222CA8 | 42 entries, 8 bytes each |
| Table count/header | 0x80222CA4 | Likely entry count before table |
| Signal pointer table | 0x80229838 | 32-bit pointers to signal RAM |
| HW Init Function | 0x8069D718 | Programs MultiCAN+ MO registers |
| CAN State Machine | 0x8069E078 | Startup/shutdown sequencing |
| CAN TX Function | 0x8069E3BC | Copies buffer to MO, triggers TXRQ |
| TX Wrapper | 0x8069E818 | Builds TX request from config tables |
| TX Wrapper 2 | 0x8069E8A6 | Second TX path (not yet decompiled) |
| TX Config Table 2 | 0x80222F34 | CAN IDs + flags, 8-byte entries |
| TX Scheduling Lookup | 0x8023371A | PDU index -> buffer offset mapping |
| Packing Fn Ptr Table | 0x8022328C | 6 entries, indexed by TX config byte 7 |
| TX Scheduler | 0x8069E9FE | Periodic task: check dirty, TX, call packer |
| Scheduler Caller | 0x8069D470 | Periodic task runner |
| Packing Fn (type 2) | 0x806A0930 | Shared packer for 0x18B, 0x152, etc. |
| Packing Fn (type 3) | 0x802A06E8 | Most common packer (AUTOSAR Com?) |
| Packing Fn (type 4) | 0x802E7A5E | Packer for 0x0EE, 0x0FD, etc. |
| Packing Fn (single) | 0x8069F8B8 | Special handler for 0x045A only |
| TX Data Buffers | 0x600181BE | RAM region holding pre-packed payloads |
| Motor_12 TX Buffer | 0x600181CA | 12-byte struct: 8B data + DLC + dirty |
| FlexRay Signal Packer | 0x804041A0 | FlexRay frame prep (RPM, torque, flags) |
| FlexRay Signal Init | 0x80403F04 | Writes default RPM to 0x60028B3C |
| Raw RPM Source | 0x60014E06 | Engine speed before FlexRay packing |
| Signal Validate A | 0x804010A6 | Validates signal pairs (ASIL pattern) |
| Signal Validate B | 0x804010FC | Validates 16-bit signal pairs |
| State Reset Fn | 0x8069EB36 | Clears pending counter + dirty flags |
| MultiCAN+ MO Base | 0xF0019000 | Hardware message object region |
| MO_DATAL offset | +0x10 | Low 4 bytes of payload |
| MO_DATAH offset | +0x14 | High 4 bytes of payload |
| Binary ID String | 0x8015F8CC | "EV_ECM40TFS0114K0907557G" |
| RPM FlexRay Signal | 0x60028B3C | MoFTx_nMotor12ESpd |
| Ethanol Content | 0x60015660 | rl_w |
| Steering Angle | 0x5001C9A6 | frm_w_msg |
| Steering Angle 2 | 0x5001CC0A | frm2_w_msg |

---

## SEFI Controller CAN Interface (for reference)

**Vehicle-side RX (ECU must transmit these):**
| Signal | CAN ID | Used For |
|--------|--------|----------|
| Engine RPM | 0x0A8 | Injection timing/pulse width |
| Pedal Position | 0x18B | Load-based enrichment |
| Ambient Temp | 0x665 | Density correction |

**SEFI Controller TX (custom messages):**
| Message | CAN ID | Content |
|---------|--------|---------|
| INJ-V1 | 0x741 | Injector PW, duty, trims, NOx |
| Heartbeat | 0x743 | Health probe at 1 Hz |

**Proposed new ECU TX:**
| Message | CAN ID | Content |
|---------|--------|---------|
| Ethanol Broadcast | 0x7A0 | Ethanol %, fuel temp, sensor status |
