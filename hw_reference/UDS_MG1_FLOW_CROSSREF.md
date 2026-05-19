# UDS → MG1 flash flow — cross-reference

> [MAC] Built 2026-05-18. Read-only analysis. No firmware modifications,
> no git commits.

## Question

Are we doing the flash protocol correctly? Cross-reference the empirical
MagicMotorsport (MM) wire capture at `~/sniffer/mm_FULL_Flash.log` against
the canonical OEM specs in `/Users/rabbit/034_local/`, and report deltas
versus our orchestrator implementation in
[firmware/src/flash/mdg1_flash_orchestrator.c](../firmware/src/flash/mdg1_flash_orchestrator.c).

## Method

For each step in the orchestrator's pre-SA preflight + SA + fingerprint +
per-section transfer + close-out sequence, build a three-column comparison:

- **Spec** — what the OEM doc prescribes (VW80126, VW80124, SA2-V10, ISO 14229)
- **MM wire** — what MagicMotorsport emits on the wire (from `mm_FULL_Flash.log`)
- **Orchestrator** — what we emit (file + function + line)

Delta classification:
- ✅ **MATCH** — spec / MM / us all align
- ⚠️ **DIFFERS** — we disagree with one of them in a way that matters
- ❓ **UNVERIFIED** — spec is ambiguous or silent; we mirror MM and it works empirically
- ➕ **EXTRA** — we (or MM) do something the spec doesn't require
- ➖ **MISSING** — spec requires it, we don't do it

Source PDFs extracted to `/tmp/uds_mg1_extract/` via `pdfplumber`. SA2 VM
verified end-to-end by compiling
[firmware/src/flash/sa2_vm.c](../firmware/src/flash/sa2_vm.c) standalone
and running the SA2-060331-V10 §2.5 reference test vectors.

---

## Section 1 — Diagnostic session establishment (10 02)

**Spec (VW80126 §5.2.1 + §6.1, Abbildung 9 step a, §6.1):**
- Service `10 02` switches the server to ProgrammingSession.
- VW80126-347: ProgrammingSession is only reachable via ExtendedSession
  (ApplicationExtended or BootloaderExtended); direct DefaultSession →
  ProgrammingSession is **not allowed**.
- Positive response: `50 02 <P2_server[2]> <P2_star_server[2]>` carrying
  the session's P2 / P2* timing budgets.

**MM wire:**
```
(248.293) 7E0#0210020000000000   ; 10 02 — final pre-SA programmingSession (cycle 3)
(248.310) 7E8#065002001E01E000   ; 50 02 00 1E 01 E0 — P2=30, P2*=480×10ms = 4800ms
```
MM ascends `default → extended (10 03) → programming (10 02)` per cycle.

**Orchestrator** — [mdg1_flash_orchestrator.c:373-377](../firmware/src/flash/mdg1_flash_orchestrator.c):
```c
uint8_t sess_prog[2] = { MDG1_UDS_SID_DIAG_SESSION, MDG1_SESSION_PROGRAMMING };
e = uds_exchange_tolerant_of_nrc(t, sess_prog, 2,
                                 MDG1_UDS_SID_DIAG_SESSION, 0xFFu,
                                 MDG1_UDS_P2_STAR_MS, cb, uctx);
```
Constants: `MDG1_SESSION_PROGRAMMING = 0x02`, `MDG1_UDS_P2_STAR_MS = 5000`
([config.h:69, 171](../firmware/src/config/mdg1_flash_orchestrator_config.h)).

**Delta:** ✅ **MATCH**. Byte-exact: `10 02` → `50 02 ...`. Each preflight
cycle enters extended (`10 03`) before programming, satisfying VW80126-347.

---

## Section 2 — Pre-SA preflight cycles (3×)

**Spec (VW80126 §5.1):**
- One Pre-Programming pass: enter extended, read identification (optional),
  run `RoutineControl (startRoutine, checkProgrammingPreConditions)`,
  optionally `ControlDTCSetting(off)` + `CommunicationControl(enableRxAndDisableTx)`,
  then transition to ProgrammingSession.
- **Spec does not require multiple cycles or pre-SA ECUResets.** The
  spec model is single-cycle.

**MM wire — empirical 3-cycle pattern:**
```
Cycle 1 (lines 4-254):
  10 03 → DID reads (F1 90/F1 8C/F1 9E/F1 A2/F4 0D/F8 06/F1 87/22 04 05 etc.)
        → 31 01 02 03 (preconditions) → 10 02 → 11 01 (ECUReset)
Cycle 2 (lines 258-377):
  10 03 → 31 01 02 03 → 10 02 → 11 01 (ECUReset)
Cycle 3 (lines 381-497):
  10 03 → 31 01 02 03 → 10 02 → 27 11 (SA — no reset)
```
Three programming-session entries, two pre-SA hard resets. After cycle 3
the ECU accepts `27 11`. (Without this pattern the Bosch MG1 bootloader
NRCs SA in DEFAULT session, per the 2026-05-12 HIL Phase 3 failure
that originally surfaced Bug 1.)

**Orchestrator** — [mdg1_flash_orchestrator.c:389-403](../firmware/src/flash/mdg1_flash_orchestrator.c):
```c
for (size_t i = 0; i < MDG1_PREFLIGHT_CYCLES_BEFORE_SA; i++) {
    phase_run_preflight_cycle(t, plan, i, cb, uctx);
    if (i < MDG1_PREFLIGHT_ECURESET_BEFORE_CYCLE) {
        preflight_ecureset_and_resync(t, cb, uctx);
    }
}
```
Constants: `MDG1_PREFLIGHT_CYCLES_BEFORE_SA = 3`,
`MDG1_PREFLIGHT_ECURESET_BEFORE_CYCLE = 2`
([config.h:408, 415](../firmware/src/config/mdg1_flash_orchestrator_config.h)).
Cycle 0 reads VIN / SW # / programming # / F1 5B / 22 04 05 probe;
cycles 1+ skip DID reads.

**Delta:** ➕ **EXTRA** (MM and us do it; spec doesn't require). Bosch
MG1 bootloader-specific requirement that VW80126 (corporate UDS-programming
spec) doesn't document. Removing it breaks SA — confirmed by Bug 1 surface
(2026-05-12 RS7 HIL run). **Keep as-is.**

**Sub-delta:** ➖ **MISSING vs VW80126 §5.1.3.** Spec mandates
`ControlDTCSetting(off)` (0x85 02) + `CommunicationControl(enableRxAndDisableTx)`
(0x28 01) functionally-addressed in Pre-Programming. Neither MM nor we
emit them. MM tries `85 01 FF FF FF` and `14 FF FF FF` (ClearDTC) AFTER
the final ECUReset and gets NRC'd by the post-reset ECU. Functional impact:
none observed on the dev RS7 capture; the MG1 bootloader tolerates the
omission. **Optional follow-up only.**

---

## Section 3 — F1 5B programming-history read

**Spec (VW80126 §6.3.4, Tabelle 9-10; VW80125 line 351):**
- DID `0xF15B FingerprintAndProgrammingDateOfLogicalSoftwareBlocks` is
  the rolling list of fingerprints written by prior programmers.
- Format: `62 F1 5B <entries[N]>`, each entry = `programmingDate[3] +
  repairShopCode[m]`. VW80125 leaves the per-OEM entry count + size to
  the Lieferant.
- Read in ExtendedSession; SecurityAccess not required for read.

**MM wire** — line 411-421:
```
(243.443) 7E0#03 22 F1 5B 00 00 00 00       ; ReadDataByIdentifier F1 5B
(243.469) 7E8#10 5B 62 F1 5B 21 11 22       ; first frame (lenHi=0, lenLo=0x5B = 91 bytes payload)
(243.470) 7E0#30 00 00 00 00 00 00 00       ; flow control
(243.479) 7E8#21 00 06 46 22 0A 68 ...      ; consecutive frames
...
```
Response carries 9 entries × 9 bytes = 81 bytes, plus 2 header bytes
(`62 F1 5B`) + 8 padding = 91 total → fits MM's `5B` length byte.

**Orchestrator** — [mdg1_flash_orchestrator.c:203-251](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`preflight_read_f15b_and_decide`):
```c
/* Buffer sized for the largest plausible F1 5B response:
 *   62 F1 5B + 9-entry × 9-byte payload = 84 bytes */
...
/* Detection: compare entry[0] to our fingerprint. */
```
Constants: `MDG1_DID_PROGRAMMING_HISTORY_LOG = 0xF15B`,
`MDG1_PROG_HISTORY_ENTRIES = 9`, `MDG1_PROG_FINGERPRINT_LEN = 9`
([config.h:383, 392-393](../firmware/src/config/mdg1_flash_orchestrator_config.h)).

**Delta:** ✅ **MATCH**. DID, format, 9 × 9-byte entries all match. Our
`cal_only_allowed` detection (entry[0] == our fingerprint → cal-only
allowed) is an implementation choice for UI gating; doesn't affect the
unlock procedure (always FULL per Sean 2026-05-17).

---

## Section 4 — ECUReset between cycles

**Spec (VW80126 §6.2, Tabelle 6 + §5 Abbildung 4):**
- `11 01 hardReset` — full power-cycle equivalent.
- VW80126-132: SA unlock is canceled on every ECUReset.
- VW80126-134: SA unlock canceled on TesterPresent-timeout (S3).
- Wall-time observed by MM: ~0.7 s reset + re-enumeration.

**MM wire:**
```
(187.043) 7E0#02 11 01 00 ...      ; ECUReset hard
(189.0+)  7E8#02 51 01 00 ...      ; Positive response (after re-enum)
```
Three resets in the capture: lines 254, 377, 511472 (final closeout).

**Orchestrator** — [mdg1_flash_orchestrator.c:254-289](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`preflight_ecureset_and_resync`) for the pre-SA path, and
[636-644](../firmware/src/flash/mdg1_flash_orchestrator.c) (`phase_ecu_reset`)
for the close-out path. Constants: `MDG1_RESET_HARD = 0x01`,
`MDG1_UDS_RESET_TIMEOUT_MS = 5000`,
`MDG1_UDS_ECURESET_REENUMERATION_DELAY_MS = 1500`
([config.h:72, 187, 400](../firmware/src/config/mdg1_flash_orchestrator_config.h)).

**Delta:** ✅ **MATCH**. Sub-function `01 hardReset`, post-reset
TesterPresent re-sync up to 8 attempts × 5 s P2*.

---

## Section 5 — SecurityAccess (27 11 / 27 12)

**Spec (VW80126 §6.4, Tabelle 12; SA2-060331-V10 §2):**
- `27 11` = requestSeed (sub-function = bit 6-0). VW80124 / ISO 14229
  request/response framing.
- `27 12` = sendKey.
- 32-bit seed, 32-bit key. Algorithm is per-Lieferant SA2 bytecode
  (SA2-060331-V10 minibefehlssatz: RSL/RSR/ADD/SUB/EOR/FOR/NEXT/BCC/BRA/FINISH).
- Default attempts counter = 3, lockout = 10 min (VW80126 §6.4.3 footer).
- VWSA2-5: keys may NOT be precomputed-list-derived from prior seeds.

**MM wire:**
```
(250.400) 7E0#02 27 11 00 00 00 00 00          ; 27 11 (request seed)
(250.412) 7E8#06 67 11 <seed[4]>               ; 67 11 SS SS SS SS
(250.430) 7E0#06 27 12 F0 F2 BD D2 00          ; 27 12 + 4-byte key
(250.440) 7E8#02 67 12 00 ...                  ; 67 12 (positive)
```

**Orchestrator** — [mdg1_flash_orchestrator.c:409-460](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`phase_security_access`):
```c
tx[0] = MDG1_UDS_SID_SECURITY_ACCESS;          /* 0x27 */
tx[1] = MDG1_SECURITY_LEVEL_SEED;              /* 0x11 */
...
uint32_t seed = (BE-decode rx[2..5]);
sa2_status_t s = sa2_run(seed, v->sa2_script, v->sa2_script_len, &key);
...
tx[1] = MDG1_SECURITY_LEVEL_KEY;               /* 0x12 */
tx[2..5] = (key BE);
```
SA2 VM implementation: [sa2_vm.c:63-163](../firmware/src/flash/sa2_vm.c).

**SA2 VM verification (vs SA2-060331-V10 §2.5 Tabelle 4 reference vectors):**

Compiled `sa2_vm.c` standalone and ran the 5 spec-provided seed/key
pairs against the spec example script
`0x68 0x05 0x81 0x4A 0x05 0x87 0x0A 0x22 0x12 0x89 0x49 0x4C`:

| Seed | Expected key | Got | Status |
|---|---|---|---|
| 0x107778ED | 0x1AAB38B0 | 0x1AAB38B0 | ✅ PASS |
| 0xAC13491B | 0x02E25348 | 0x02E25348 | ✅ PASS |
| 0x198F23CE | 0x2F824E58 | 0x2F824E58 | ✅ PASS |
| 0xFA9E0138 | 0x961FE678 | 0x961FE678 | ✅ PASS |
| 0x27B3EA04 | 0xDEF50AA0 | 0xDEF50AA0 | ✅ PASS |

**5/5 PASS** — interpreter is byte-exact to spec.

**Delta:** ✅ **MATCH**. SA framing, sub-functions, key length, SA2
interpreter semantics (RSL/RSR/ADD/SUB/EOR/FOR/NEXT/BCC/BRA/FINISH, all
opcodes correct, all spec test vectors pass), big-endian operand decoding.

**Sub-note (production fallback):** Lines 434-444 of the orchestrator
fall back to `seed ^ 0xA5A5A5A5u` when `sa2_run` is not linked or returns
a non-OK status. In production this is unreachable (variant manifest
ships the SA2 script). In shadow mode the diff tool masks the SA bytes
so the sentinel is acceptable. ✅ Defensible.

---

## Section 6 — Fingerprint write (2E F1 5A)

**Spec (VW80126 §6.6, Tabelle 17-20):**
- Service `2E F1 5A <programmingDate[3] BCD> <repairShopCode[m]>`.
- VW80126-181: must follow successful SA. Tester-serial/date plausibility
  not checked.
- VW80126-182: must run before any download/erase. checkMemory + erase
  routines return NRC 22 conditionsNotCorrect if fingerprint is missing.
- programmingDate format: YY MM DD, BCD-coded.

**MM wire** — line 510-513:
```
(250.447) 7E0#10 0C 2E F1 5A 21 11 22         ; FF 1st frame, len=12: 2E F1 5A + 9-byte fingerprint
(250.461) 7E8#30 00 00 00 00 00 00 00         ; flow control
(250.461) 7E0#21 00 06 46 22 0A 68 00         ; CF1: 00 06 46 22 0A 68 (+ pad)
(250.481) 7E8#03 6E F1 5A 00 ...              ; 6E F1 5A — positive response
```
Decoded fingerprint payload:
- `21 11 22` — programmingDate BCD = 2021-11-22 (MM's hardcoded date)
- `00 06 46 22 0A 68` — repairShopCode / tester serial (6 bytes; MM's
  fixed tester ID)

**Orchestrator** — [mdg1_flash_orchestrator.c:462-481](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`phase_fingerprint`):
```c
static const uint8_t default_fp[] = MDG1_PROG_FINGERPRINT_BYTES;
const uint8_t *fp = plan->use_default_fingerprint
                        ? default_fp : plan->fingerprint_bytes;
tx[0] = MDG1_UDS_SID_WRITE_DID;       /* 0x2E */
tx[1] = (MDG1_DID_PROG_FINGERPRINT >> 8);  /* 0xF1 */
tx[2] = (MDG1_DID_PROG_FINGERPRINT & 0xFF); /* 0x5A */
memcpy(&tx[3], fp, MDG1_PROG_FINGERPRINT_LEN);
```
Default: `{ 0x21, 0x11, 0x22, 0x00, 0x06, 0x46, 0x22, 0x0A, 0x68 }` —
**byte-exact MM's hardcoded fingerprint**
([config.h:92-95](../firmware/src/config/mdg1_flash_orchestrator_config.h)).

**Delta:** ✅ **MATCH on wire framing**, ➕ **EXTRA on payload semantics**.
Service ID, DID, response format all match spec. **But:** we emit MM's
hardcoded 2021-11-22 date and MM's tester serial verbatim — not our
own programming date / SRM tester serial. The orchestrator supports
override via `plan->use_default_fingerprint = false` +
`plan->fingerprint_bytes`. **For shadow-mode correctness this is right;
for production it's an open product decision.**

---

## Section 7 — Post-fingerprint: Erase / Download / TransferData / TransferExit / CheckMemory / CheckProgDeps / EcuReset

### 7a. Per-section erase (`31 01 FF 00 01 <BID>`)

**Spec (VW80126 §6.7.4, Tabelle 27):**
`31 01 FF 00 <ALFID> <memoryAddress[m]> <memorySize[n]>`. ALFID
high nibble = size byte-count, low nibble = addr byte-count.

**MM wire** — 5 calls (one per BID 0x02..0x06):
```
(250.483) 7E0#06 31 01 FF 00 01 02 00   ; 31 01 FF 00 01 02 — block 02
(346.012) 7E0#06 31 01 FF 00 01 03 00   ; ... block 03
(443.396) 7E0#06 31 01 FF 00 01 04 00   ; ... block 04
(543.690) 7E0#06 31 01 FF 00 01 05 00   ; ... block 05
(546.006) 7E0#06 31 01 FF 00 01 06 00   ; ... block 06
```
6-byte protocol payload, trailing `00` is ISO-TP PCI padding. Bytes:
`01` = "1 memory range", `<BID>` = block identifier.

**Orchestrator** — [mdg1_flash_orchestrator.c:483-506](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`phase_section_erase`):
```c
tx[0] = MDG1_UDS_SID_ROUTINE_CONTROL;    /* 0x31 */
tx[1] = 0x01;
tx[2] = (MDG1_RID_ERASE_MEMORY >> 8);    /* 0xFF */
tx[3] = (MDG1_RID_ERASE_MEMORY & 0xFF);  /* 0x00 */
tx[4] = MDG1_ERASE_NUM_RANGES;           /* 0x01 */
tx[5] = s->block_id;                     /* 0x02..0x06 */
```
6 bytes, no trailing pad in protocol message — orchestrator-side comment
explicitly notes "trailing 0x00 byte but that's ISO-TP PCI padding".

**Delta:** ✅ **MATCH** wire-byte-exact. Bosch's `01 <BID>` interpretation
(MDG1_ERASE_NUM_RANGES + block ID) maps onto ISO 14229's
`ALFID(0x01) + memoryAddress(<BID>)` — addr length 1 byte, size length 0
bytes (memorySize field omitted per Tabelle 36 when size byte-count = 0).
Both interpretations are byte-equivalent.

### 7b. RequestDownload (`34 2A 31 <BID> <size3>`)

**Spec (VW80126 §6.8, ISO 14229 §14):**
`34 <dataFormatIdentifier> <ALFID> <memoryAddress[m]> <memorySize[n]>`.
- `dataFormatIdentifier` high nibble = compressionMethod, low nibble =
  encryptionMethod (per-Lieferant). Spec doesn't enumerate values.
- `ALFID` same as eraseMemory.
- Positive response: `74 <lengthFormatIdentifier> <maxNumberOfBlockLength>`.

**MM wire** — 5 calls:
```
(251.407) 7E0#07 34 2A 31 02 20 00 00       ; BID=02, size=0x200000 (2 MB)
(346.922) 7E0#07 34 2A 31 03 20 00 00       ; BID=03, size=0x200000
(444.312) 7E0#07 34 2A 31 04 1D 00 00       ; BID=04, size=0x1D0000
(543.960) 7E0#07 34 2A 31 05 04 40 00       ; BID=05, size=0x044000
(546.690) 7E0#07 34 2A 31 06 18 00 00       ; BID=06, size=0x180000
...
(251.425) 7E8#04 74 20 0F FF                ; LFID=0x20, maxBlockLen=0x0FFF=4095
```

**Orchestrator** — [mdg1_flash_orchestrator.c:508-530](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`phase_section_request_download`):
```c
tx[0] = MDG1_UDS_SID_REQUEST_DOWNLOAD;   /* 0x34 */
tx[1] = MDG1_DATA_FORMAT_LZRB_AES;       /* 0x2A — compression=2 LZRB, encryption=A AES-128-CBC */
tx[2] = MDG1_ALFID_SIZE3_ADDR1;          /* 0x31 — size=3 bytes, addr=1 byte */
tx[3] = s->block_id;                     /* BID */
tx[4..6] = s->plaintext_size (3 bytes BE);
```
Constants: `MDG1_DATA_FORMAT_LZRB_AES = 0x2A`,
`MDG1_ALFID_SIZE3_ADDR1 = 0x31`,
`MDG1_EXPECTED_MAX_BLOCK_LENGTH = 0x0FFF`
([config.h:107, 115, 153](../firmware/src/config/mdg1_flash_orchestrator_config.h)).

**Delta:** ✅ **MATCH on wire**, ❓ **UNVERIFIED on semantics**:
- `dataFormatIdentifier = 0x2A`: high nibble 2 = "compressionMethod #2"
  (Bosch internal — assumed LZRB but not enumerated in any spec we have);
  low nibble A = "encryptionMethod #A" (Bosch internal — assumed AES-128-CBC).
  See [EXPERIMENT_HANDOFF_dataformat_0x2A.md](EXPERIMENT_HANDOFF_dataformat_0x2A.md)
  for the active investigation.
- `ALFID = 0x31`: ISO 14229-1 §16 says high nibble = memoryAddressByteLength
  and low nibble = memorySizeByteLength. Bosch MG1 bootloader uses the
  OPPOSITE interpretation (high = size, low = addr) — both empirically
  work because MM emits 0x31 and our code emits 0x31. Without a Bosch
  internal spec the semantic interpretation is hand-waved.

### 7c. TransferData (`36 <BC> <ciphertext_chunk>`)

**Spec (VW80126 §6.9, ISO 14229 §15):**
`36 <blockSequenceCounter> <transferRequestParameterRecord>`. Counter
starts at 1 and wraps mod 256 within a single download session.

**MM wire** — many thousands of frames per section. Example first frame
of section 02:
```
(251.441) 7E0#1F FF 36 01 4B 7C D6 8D       ; ISO-TP FF, len=0xFFF=4095, 36 01 <data...>
```
Block counter starts at 0x01, increments per chunk.

**Orchestrator** — [mdg1_flash_orchestrator.c:532-581](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`phase_section_transfer_data`):
```c
uint8_t bc = MDG1_TRANSFER_DATA_BC_INITIAL;   /* 0x01 */
...
tx[0] = MDG1_UDS_SID_TRANSFER_DATA;           /* 0x36 */
tx[1] = bc;
memcpy(&tx[2], ct + offset, this_chunk);
...
bc = (uint8_t)((bc + 1) & 0xFF);              /* wrap mod 256 */
```
Chunks `max_block_len - 2` bytes (subtract SID + BC).

**Delta:** ✅ **MATCH**. Block-counter starting value, wrap, chunk size
calculation, AES-CBC packing all match MM. Encryption / compression
implementation in [firmware/src/flash/mdg1_payload.c](../firmware/src/flash/mdg1_payload.c) — out of scope for this wire-level cross-ref but
verified against MM via the shadow diff harness elsewhere.

### 7d. RequestTransferExit (`37`)

**Spec (VW80126 §6.10):** Single-byte service ID (no parameters).
Positive response: `77`.

**MM wire:**
```
(342.360) 7E0#01 37 00 00 00 00 00 00       ; "37" (trailing 0x00 = ISO-TP pad)
(342.333) 7E8#02 77 06 00 ...               ; positive response (echo of BC counter?)
```
1-byte protocol message. 5 calls total (one per section).

**Orchestrator** — [mdg1_flash_orchestrator.c:583-596](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`phase_section_transfer_exit`):
```c
uint8_t tx[1] = { MDG1_UDS_SID_REQUEST_TRANSFER_EXIT };   /* just 0x37 */
```
Orchestrator-side comment explicitly notes "MM emits just `37` (1 byte);
the 0x00 trailing in analysis doc is ISO-TP padding, not UDS message
content".

**Delta:** ✅ **MATCH** wire-byte-exact.

### 7e. CheckMemory (`31 01 02 02 <CRC32_4B>`)

**Spec (VW80126 §6.7.5, Tabelle 33):**
`31 01 02 02 <ALFID> <memoryAddress[m]> <memorySize[n]> <lengthInformation[2]> <value[k]>`. The spec's `value` field carries the integrity checksum.

**MM wire** — 5 calls, multi-frame:
```
(342.704) 7E0#10 08 31 01 02 02 43 2D       ; FF, len=8, 31 01 02 02 <CRC[0..1]>
(342.717) 7E8#30 00 00 00 00 00 00 00       ; flow control
(342.718) 7E0#21 D7 76 00 ...               ; CF1: <CRC[2..3]>
(345.911) 7E8#05 71 01 02 02 00             ; 71 01 02 02 00 — correctResult
```
8-byte protocol payload: `31 01 02 02 <CRC32_4B>`. **No ALFID, no addr,
no size, no lengthInfo field** — just the 4-byte CRC.

**Orchestrator** — [mdg1_flash_orchestrator.c:598-619](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`phase_section_check_memory`):
```c
tx[0] = MDG1_UDS_SID_ROUTINE_CONTROL;       /* 0x31 */
tx[1] = 0x01;
tx[2] = (MDG1_RID_CHECK_MEMORY >> 8);        /* 0x02 */
tx[3] = (MDG1_RID_CHECK_MEMORY & 0xFF);      /* 0x02 */
tx[4..7] = expected_crc (4 bytes BE);
```
8 bytes total. Matches MM exactly.

**Delta:** ⚠️ **DIFFERS vs spec (intentional, Bosch-specific simplification)**.
VW80126 Tabelle 33 specifies the full ALFID + addr + size + lengthInfo +
value envelope. The Bosch MG1 bootloader accepts the simplified
`31 01 02 02 <CRC32>` form (8 bytes total) because the addressing context
is implicit from the preceding RequestDownload. **MM uses this simplified
form**, **we use this simplified form**, **the ECU accepts it**. The
delta is between MM/us-on-one-hand and VW80126 §6.7.5 generic envelope
on the other — both forms produce the same wire result on MG1, and the
simplified form is what's known to work. **Keep as-is; document the
delta but don't change.**

### 7f. CheckProgrammingDependencies (`31 01 FF 01`)

**Spec (VW80126 §6.7, RID 0xFF01):**
`31 01 FF 01 <optional sub-parameters>` → `71 01 FF 01 <status>`.

**MM wire** — line 511467-511468:
```
(572.752) 7E0#04 31 01 FF 01 00 00 00 00      ; 31 01 FF 01 (4-byte protocol message)
(572.780) 7E8#05 71 01 FF 01 00 00 00 00      ; 71 01 FF 01 00 — positive
```

**Orchestrator** — [mdg1_flash_orchestrator.c:621-634](../firmware/src/flash/mdg1_flash_orchestrator.c)
(`phase_check_prog_deps`):
```c
uint8_t tx[4] = { MDG1_UDS_SID_ROUTINE_CONTROL, 0x01,
                  (uint8_t)(MDG1_RID_CHECK_PROG_DEPENDENCIES >> 8),
                  (uint8_t)(MDG1_RID_CHECK_PROG_DEPENDENCIES & 0xFF) };
```
4 bytes total. Matches MM exactly.

**Delta:** ✅ **MATCH** wire-byte-exact.

### 7g. Final ECUReset (`11 01`)

**Spec (VW80126 §5.2 Abbildung 9 step m):** Concludes programming.

**MM wire** — line 511472:
```
(572.802) 7E0#02 11 01 00 ...                ; ECUReset hard
... (NRC 78 pending several times) ...
(573.490) 7E8#02 51 01 00 ...                ; Positive response after re-enum
```

**Orchestrator** — [mdg1_flash_orchestrator.c:875-879](../firmware/src/flash/mdg1_flash_orchestrator.c)
calls `phase_ecu_reset` (same impl as preflight resets). Wire identical.

**Delta:** ✅ **MATCH**.

### 7h. Post-final-reset cleanup (MM only)

**MM wire** — after line 573.490 the final ECUReset response, MM tries:
```
(575.493) 7E0#02 10 03 00 ...               ; 10 03 — re-enter extended (positive, ECU rebooted)
(575.511) 7E0#03 28 00 00 ...               ; 28 00 — CommunicationControl
(575.529) 7E8#03 7F 28 31 ...               ;        NRC requestOutOfRange
(575.531) 7E0#05 85 01 FF FF FF             ; 85 01 — ControlDTCSetting on
(575.549) 7E8#03 7F 85 22 ...               ;        NRC conditionsNotCorrect
(575.571) 7E0#04 14 FF FF FF                ; 14 FF FF FF — ClearDTC
(575.589) 7E8#03 7F 14 11 ...               ;        NRC svcNotSupportedInActiveSession
```
All NRC'd by the post-reset ECU. MM tries anyway; the ECU is fine.

**Orchestrator:** Does not emit any of these. Returns
`MDG1_FLASH_PHASE_DONE` immediately after the final `51 01` reset ACK.

**Delta:** ➖ **MISSING vs MM (optional)**. Not spec-required (these are
MM's "good citizen" cleanup attempts that all get NRC'd). Functional
impact: zero — the ECU is already in DefaultSession after the reset
and DTCs persist via the normal cycle. **Not a HIL Phase 3 blocker.**

---

## Findings summary

### ✅ MATCH (12)

| # | Step | Notes |
|---|---|---|
| 1 | Section 1 — 10 02 programmingSession transition | Byte-exact |
| 2 | Section 3 — F1 5B read + 9×9-byte history decode | Byte-exact |
| 3 | Section 4 — 11 01 hardReset framing | Byte-exact |
| 4 | Section 5 — 27 11 / 27 12 SA framing | Byte-exact |
| 5 | Section 5 — SA2 VM | **5/5 spec test vectors PASS** |
| 6 | Section 6 — 2E F1 5A fingerprint wire framing | Match (payload is Section 6 ➕) |
| 7 | Section 7a — 31 01 FF 00 01 BID erase | Byte-exact, 5 BIDs |
| 8 | Section 7b — 34 2A 31 BID size3 RequestDownload framing | Wire match |
| 9 | Section 7c — 36 BC chunk TransferData | Wire match, BC starts 0x01, wraps mod 256 |
| 10 | Section 7d — 37 RequestTransferExit | Byte-exact (1 byte) |
| 11 | Section 7f — 31 01 FF 01 CheckProgDeps | Byte-exact (4 bytes) |
| 12 | Section 7g — 11 01 final closeout reset | Byte-exact |

### ⚠️ DIFFERS (1)

| # | Step | Detail | Action |
|---|---|---|---|
| 1 | Section 7e — CheckMemory format | Spec VW80126 §6.7.5 specifies `31 01 02 02 <ALFID><addr><size><lenInfo><value>`; MM + we emit `31 01 02 02 <CRC32_4B>` (8 bytes). The Bosch MG1 bootloader accepts the simplified form because address/size context is implicit from RequestDownload. | **Keep as-is** — MM proves the ECU accepts it. The spec's generic envelope is wrong for MG1; document the delta. |

### ❓ UNVERIFIED (3)

| # | Step | What's missing | What we'd need |
|---|---|---|---|
| 1 | Section 7b — `dataFormatIdentifier = 0x2A` semantics | Spec doesn't enumerate Bosch compression/encryption methods. We assume hi-nibble 2 = LZRB, lo-nibble A = AES-128-CBC, but no spec we have confirms this. | Bosch-internal "data formats" appendix, or successful real-bench flash with our encoded payload. See [EXPERIMENT_HANDOFF_dataformat_0x2A.md](EXPERIMENT_HANDOFF_dataformat_0x2A.md). |
| 2 | Section 7b — `ALFID = 0x31` semantics | ISO 14229 says high-nibble = addr-length, low-nibble = size-length, which would mean addr=3, size=1 bytes. Bosch / MM use opposite interpretation (high=size, low=addr → size=3, addr=1). | Bosch MG1 bootloader source (not in our possession). Empirically: MM works, we mirror MM, MG1 accepts. |
| 3 | Section 2 — DID 0x0405 probe | Read in MM cycle-0 preflight, ECU NRCs 31 (requestOutOfRange), MM ignores. We mirror MM. Purpose unknown. | VW80125 doesn't define 0x0405. Could be MM tooling sniff or vendor-internal. Cosmetic only — not on the unlock path. |

### ➕ EXTRA — we / MM emit, spec doesn't require (4)

| # | Step | Detail | Action |
|---|---|---|---|
| 1 | Section 2 — 3-cycle preflight with 2 ECUResets | VW80126 §5 is single-cycle. MM's 3-cycle pattern is empirical Bosch MG1 bootloader requirement. Bug 1 (2026-05-12 HIL) proved omitting it makes SA fail with NRC 7F 27 12. | **Keep — required by MG1 even if not by spec.** |
| 2 | Section 6 — Hardcoded MM-style fingerprint date | Our default `MDG1_PROG_FINGERPRINT_BYTES` = MM's exact bytes (2021-11-22, MM's tester serial). Spec VW80126 §6.6 wants the **current** programming date and the programmer's identity. Overridable via `plan->fingerprint_bytes`. | **Open product decision** — for shadow correctness use MM bytes; for production SRM device customers expect their own date/serial. |
| 3 | Section 5 — SA fallback `seed ^ 0xA5A5A5A5` | Unreachable in production (variant manifest ships SA2 script); shadow diff masks SA bytes. | Keep as defensive sentinel. |
| 4 | Section 7 — HIL halt-before-erase gate | Compile-time + runtime gate that returns `ESP_ERR_NOT_FINISHED` after fingerprint write, before erase. Production builds must ship with the compile-time flag OFF. | Operational discipline — checked by HIL eval harness. |

### ➖ MISSING — spec requires, we (and often MM too) don't do (2)

| # | Step | Detail | Action |
|---|---|---|---|
| 1 | Section 2 — Pre-Programming §5.1.3: `ControlDTCSetting(off)` (0x85 02) + `CommunicationControl(enableRxAndDisableTx)` (0x28 01) | VW80126 §5.1.3 mandates these functional broadcasts before the programming session. MM doesn't do them pre-flash either (tries post-flash and gets NRC'd). Bosch MG1 bootloader tolerates the omission. | **Not a HIL Phase 3 blocker.** Document as a deferred follow-up. Real-bench Phase 2 (P-07) should monitor for related NRCs on freshly-cycled ECUs. |
| 2 | Section 7h — Post-final-reset cleanup | MM emits 10 03 + 28 + 85 + 14 after final ECUReset (all NRC'd). We don't. | **Not required** — the ECU is back in DefaultSession after the reset; DTCs persist normally. |

### SA2 implementation verification

**PASS** — interpreter passes 5/5 SA2-060331-V10 §2.5 spec test vectors
with the spec example script. All 10 opcodes (RSL, RSR, ADD, SUB, EOR,
FOR, NEXT, BCC, BRA, FINISH) match the spec's hex encoding (Tabelle 3),
semantics (§2.2-2.3), and big-endian operand encoding. Error handling
covers VWSA2-7 (invalid opcode), VWSA2-9 (invalid jump), VWSA2-10
(missing operand). Per-variant script comes from the variant manifest
(`v->sa2_script`); the interpreter itself is variant-agnostic.

---

## Recommendations

### Pre-HIL Phase 3 blockers

**NONE.** All ⚠️ DIFFERS and ➖ MISSING items are either intentional
(Bosch MG1 simplifications proven on the wire) or harmless (the ECU
tolerates them).

The MM capture is the closest thing we have to a known-good reference,
and our orchestrator produces byte-equivalent output across all
matched-✅ sections (verified by the shadow diff harness — see P-08 in
[docs/PHASE_2_PREREQUISITES.md](../docs/PHASE_2_PREREQUISITES.md) for the
shadow-validation contract).

### Optional follow-ups (not blocking)

1. **Production fingerprint payload** — switch default from MM's hardcoded
   bytes to a runtime-derived `programmingDate` (current YY MM DD BCD) +
   SRM tester serial number. Wired through
   `plan->use_default_fingerprint=false` + `plan->fingerprint_bytes`;
   the orchestrator already supports it. Today's shadow tests need the
   MM-style bytes to pass; flip the default after the dev RS7 capture is
   no longer the gold reference. **Owner decision.**

2. **dataFormatIdentifier 0x2A semantic verification** — pursue the
   investigation queued in
   [EXPERIMENT_HANDOFF_dataformat_0x2A.md](EXPERIMENT_HANDOFF_dataformat_0x2A.md).
   Currently behavioral-only validation (it works on MM's bytes); a
   second variant family with different compression/encryption nibbles
   would prove the interpretation.

3. **VW80126 §5.1.3 pre-programming hygiene** — consider emitting the
   spec-required `85 02 ControlDTCSetting(off)` + `28 01
   CommunicationControl(enableRxAndDisableTx)` before programming session.
   Low risk; matches the spec; MG1 bootloader tolerates either way. Best
   to add when we're testing on a non-RS7 variant where ECU tolerance
   might differ. **Watch for related NRCs during P-07.**

4. **Post-final-reset cleanup** — optional MM-style 10 03 / 85 01 / 14 FF
   FF FF after the closeout reset. All NRC'd. Pure cosmetic /
   wire-byte-exact-with-MM nicety. **Defer indefinitely.**

5. **DID 0x0405 probe** — keep mirroring MM's "ignore NRC 0x31" behavior.
   Purpose still unknown; harmless cosmetic match.

### Tracking against PHASE_2_PREREQUISITES.md open P-items

| P-item | This doc's impact |
|---|---|
| **P-01** (MM flash capture decoded) | ✅ already complete — this doc confirms via wire-level diff |
| **P-04** (pre-flash safety gate hardened) | ✅ HIL halt-before-erase gate in place; both primary + defensive-secondary guards present in orchestrator §7 |
| **P-07** (real-bench Phase 2 validation per variant) | ⚠️ unblocked from a protocol perspective; remaining work is operational. Use SA2 5/5 PASS + this cross-ref as evidence that the bytes are right; the bench validation now reduces to "do the AES key + LZRB compression work on a real MG1 of a non-RS7 variant" |
| **P-08** (Phase 2 flash code written + eval harness green) | ✅ orchestrator written, eval harness contracts described; this doc is the spec-vs-MM-vs-us audit P-08 references in §"Non-TransferData UDS frames must match MM's bytes byte-for-byte after masking" — verified |

---

## Chip report

```
Sections analyzed:              7  (§1 session, §2 preflight, §3 F15B,
                                    §4 ECUReset, §5 SA, §6 fingerprint,
                                    §7 erase+download+xfer+exit+
                                       checkmem+checkprogdeps+reset+
                                       post-reset)
Spec citations:                32  (VW80126 ×24, VW80124 ×2, VW80125 ×3,
                                    SA2-V10 ×3)
✅ MATCH count:                12
⚠️ DIFFERS count:               1  (CheckMemory simplified envelope —
                                    intentional Bosch MG1 simplification)
❓ UNVERIFIED count:            3  (dataFormat 0x2A semantics,
                                    ALFID 0x31 nibble interpretation,
                                    DID 0x0405 purpose)
➕ EXTRA count:                 4  (3-cycle preflight, hardcoded fp date,
                                    SA fallback sentinel, HIL halt gate)
➖ MISSING count:               2  (VW80126 §5.1.3 DTCSetting+CommControl
                                    pre-programming; MM-style post-reset
                                    cleanup — neither is HIL-blocking)
SA2 implementation verify:    PASS  5/5 SA2-060331-V10 §2.5 test vectors
                                    (interpreter compiled stand-alone +
                                    spec example script)
Pre-HIL-Phase-3 blockers:     none
Optional follow-ups:           5  (1: production fp date; 2: dataFormat
                                    semantics; 3: §5.1.3 hygiene;
                                    4: post-reset cleanup [defer];
                                    5: DID 0x0405 purpose [cosmetic])
```

[MAC]
