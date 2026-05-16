# MagicMotorsport MDG1 Flash — Capture Analysis

**Status:** initial protocol decoding from two CAN-bus captures of MagicMotorsport (MM) writing a 4K0907557G (MDG1, RS7 4.0 TFSI) ECU. Source captures, the binary that was flashed, and the MM console output are in `/Users/rabbit/sniffer/` (local-only; not committed). This document is the structured analysis of what those captures prove and what they do not.

This is intended as the working reference for the FUTUNER Phase 2 flash writer (`firmware/src/flash/mdg1_flash.{c,h}`). It supersedes any prior assumption about MDG1 addressing in `docs/PHASE_2_PREREQUISITES.md` — see "Confirmed protocol → RequestDownload" below.

---

## 1. Source artifacts

| File | Size | Role |
|------|------|------|
| `mm_connect.log` | 391 lines | Preflight only. Session entry + ID reads + Programming Preconditions routine + two ECU resets. **No SecurityAccess, no flash data.** |
| `mm_MAPS_upload.log` | 37,622 lines, ~47 s wall clock | Single-section "Write Maps" — CAL region only (block ID 0x06, 1.5 MB plaintext). |
| `mm_FULL_Flash.log` | 511,495 lines, ~5 min 51 s | Five-section "Write Maps & Code" — ASW1, ASW2, ASW3, CBOOT, CAL. |
| `WUAPCBF28NN902533_4K0907557G__0003.bin` | 8,388,608 B (8 MiB) | The exact binary MM uploaded in both captures. VIN `WUAPCBF28NN902533`, box `4K0907557G`, SW `0003`. |
| `maps only output mm.txt`, `maps and code mm output.txt` | tiny | MM console captions (gave us the section→address table). |

All traffic is on `0x7E0 / 0x7E8` (single MDG1 engine ECU). The J533 gateway broadcast IDs (`0x700`, `0x17FC0076`, `0x17FE009C`, `0x17FE0085`, `0x77C`, `0x77D`, `0x780`, `0x7E9`) appear during ECU reset enumeration but **no diagnostic traffic ever goes to `0x7DF` or to other 0x7E1–0x7E7 addresses.** MM honors the same "0x7E0 only" rule the FUTUNER project enforces.

---

## 2. Confirmed protocol — what every capture proves

Every byte below is sourced from the captures. Service IDs are UDS (ISO 14229).

### 2.1 Preflight (both flash logs do this identically before any crypto)

1. Tester present `3E 00` on 0x7E0.
2. `10 03` — Extended diagnostic session. ECU responds `50 03 00 1E 01 E0`.
3. Many `22 Fxxx` identifier reads, including:
   - `F190` VIN, `F19E` ECU SW#, `F1A2`, `F15B` programming-attempt history (9-byte rolling log), `F191`, `F1A3`, `F187`, `F189`, `F1F4` bootloader rev, `F197`, `F1AD`, `F17C`, `F1A5`, `F18C`, `F186`, `F44`, `F40D`, `F806`, `2103F`.
4. `31 01 02 03` — RoutineControl Start, **RID 0x0203 = Programming Preconditions Check**. ECU returns `7F 31 78` (response pending) for ~3.6 s before `71 01 02 03 00`.
5. `14 FF FF FF` — ClearDTC. **J533 rejects this with `7F 14 11`** (serviceNotSupported in this session). This is normal — MM ignores it.
6. `10 02` — Programming session request. ECU returns `7F 10 78` twice (~1 s each), then positive `50 02 00 32 01 F4` (P2=50 ms, P2*=5000 ms).
7. `11 01` — ECUReset hard. After ~1 s the ECU re-enumerates on its broadcast IDs and returns `51 01`.
8. MM repeats steps 2–7 a second time. After the second reset cycle, the ECU is in programming session and ready for SecurityAccess.

### 2.2 SecurityAccess — single unlock, persists for the whole session

- Request: `27 11` (1-byte sub-function, level 0x11).
- Response: `67 11 <4-byte seed>`.
- Key: `27 12 <4-byte key>`.
- Response: `67 12` (no payload — positive ack).

**Golden vectors captured:**

| Seed (4 B)   | Key (4 B)    | Source log         |
|--------------|--------------|---------------------|
| `C3 61 B0 58`| `F0 F2 BD D2`| `mm_FULL_Flash.log` |
| `16 8B C5 E2`| `F1 CB 90 0F`| `mm_MAPS_upload.log`|

The seed→key transform is the VAG/Bosch **SA2 bytecode VM** used for MDG1/MD1/MED17. The VM bytecode is per-ECU-variant and is shipped in the box-code `.a2l/.cff` or in MM's database. `firmware/src/flash/sa2_vm.{c,h}` exists in this repo (untracked) — these two pairs are the test vectors to validate it against.

The SecurityAccess unlock is performed **once** in each session and stays valid for all five sections of the full flash. There is no re-auth between sections; only the 1 Hz `3E 80` tester-present keepalive (from gateway address `0x700`) keeps the session alive.

### 2.3 Fingerprint write (mandatory before erase)

After SecurityAccess succeeds:

```
2E F1 5A 21 11 22 00 06 46 22 0A 68
```

WriteDataByIdentifier (`2E`), DID `0xF15A`, 9-byte programming fingerprint. The value MM writes (`21 11 22 00 06 46 22 0A 68`) is an entry in the same format that the ECU exposes via `22 F15B` as a rolling history of all prior programming attempts — i.e. MM is appending its tester signature/date stamp. ECU acks `6E F1 5A`.

If you skip this write, MDG1 bootloaders typically reject the next `31 01 FF 00` with NRC `0x24` (requestSequenceError) or `0x33` (securityAccessDenied). The fingerprint must be written between SecurityAccess and EraseMemory.

### 2.4 Per-section flash cycle

The same five steps run for each section. In a full flash, five passes; in maps-only, one pass.

#### 2.4.1 EraseMemory

```
31 01 FF 00 01 <BID> 00
```

RoutineControl Start, **RID 0xFF00 = EraseMemory**. Params are:
- `01` — fixed (likely "number of blocks to erase = 1").
- `<BID>` — 1-byte logical block ID (see table 2.5).
- `00` — trailing padding inside the 6-byte PCI payload (or a second routine-control flag that is always 0 in these captures).

ECU returns `7F 31 78` (NRC 0x78, response pending) repeatedly while it erases, then `71 01 FF 00 00` (status byte = 0x00 = success). Erase wall time is 0.3–0.9 s per section — fast enough to suggest the ECU may stage the erase in a RAM buffer rather than committing to physical flash immediately (see "Uncertain" §3.1).

#### 2.4.2 RequestDownload

```
34 2A 31 <BID> <size_3B>
```

- `34` — RequestDownload service.
- `2A` — dataFormatIdentifier. High nibble `2` = compressionMethod 2 (Bosch LZ — matches the staged `firmware/src/flash/lzrb.{c,h}`). Low nibble `A` = encryptionMethod 0xA (Bosch AES variant; specifics in §3.2).
- `31` — addressAndLengthFormatIdentifier. High nibble `3` = memorySize length = 3 bytes. Low nibble `1` = memoryAddress length = 1 byte.
- `<BID>` — 1-byte logical block ID (NOT a physical address).
- `<size_3B>` — 3-byte big-endian PLAINTEXT size of the section (uncompressed).

ECU response:

```
74 20 0F FF
```

- `74` — positive response.
- `20` — lengthFormatIdentifier (maxNumberOfBlockLength is 2 bytes wide).
- `0F FF` — **maxNumberOfBlockLength = 0x0FFF = 4095**. This is the maximum bytes per `36` PCI payload (service byte + counter byte + data, so ~4093 data bytes per TransferData).

This response was identical for all five sections in the FULL flash.

#### 2.4.3 TransferData

```
36 <BC> <up to 4093 bytes of compressed+encrypted data>
```

- `36` — TransferData service.
- `<BC>` — 1-byte blockSequenceCounter. Starts at `0x01` for the first block of each section and increments modulo 256 (so block #256 has counter `0x00`, block #257 has `0x01`, etc.). The counter **resets to `0x01` at the start of every new section**.
- Each block is sent as a single ISO-TP segmented message: one FirstFrame `1F FF 36 <BC> <6 data bytes>` followed by ~584 ConsecutiveFrames (sequence 0x21–0x2F wrap) at maximum bus speed.
- ECU response per block: `02 76 <BC>` (positive ack with counter echoed).
- Flow control from ECU is `30 00 00`: BS=0 (unlimited block size, no further FC needed), STmin=0 (no inter-frame spacing required). MM transmits ConsecutiveFrames roughly 500 µs apart.

**CAL section breakdown (from `mm_MAPS_upload.log`):** 64 TransferData blocks (counters 0x01–0x40), ~262 KB of compressed/encrypted bytes on the wire to deliver 1,572,864 bytes (1.5 MB) of plaintext. Compression ratio ≈ 5.8× for calibration data.

**Wall-clock bandwidth:** CAL TransferData phase ran ~21.9 s for ~262 KB on the wire = ~12 KB/s. That's ~24 % of theoretical 500 kbps CAN, the rest is ISO-TP overhead and per-frame inter-message arbitration.

#### 2.4.4 RequestTransferExit

```
37
```

No parameters. ECU returns `7F 37 78` (response pending) for ~1.3 s, then `77 00`. The long delay is consistent with the ECU performing a final integrity step at the end of each section (likely flushing the staged buffer to physical flash, see §3.1).

#### 2.4.5 CheckMemory

```
31 01 02 02 <CRC32_4B>
```

RoutineControl Start, **RID 0x0202 = CheckMemory**. The 4-byte parameter is the section's expected checksum.

**Confirmed: the checksum is plain `zlib.crc32` (IEEE 802.3 polynomial 0xEDB88320, init 0xFFFFFFFF, reflected, final XOR 0xFFFFFFFF) computed over the PLAINTEXT bytes of the section in the binary file.** Every captured CheckMemory CRC reproduces exactly when run through Python `zlib.crc32` on the appropriate slice of `WUAPCBF28NN902533_4K0907557G__0003.bin`:

| Section | Block ID | File offset range | Captured CRC | `zlib.crc32` of slice |
|---------|----------|-------------------|--------------|------------------------|
| ASW1    | 0x02     | 0x200000–0x400000 | `43 2D D7 76`| `432DD776` ✓ |
| ASW2    | 0x03     | 0x400000–0x600000 | `43 2D D7 76`| `432DD776` ✓ |
| ASW3    | 0x04     | 0x630000–0x800000 | `BD 42 0C C9`| `BD420CC9` ✓ |
| CBOOT   | 0x05     | 0x01C000–0x060000 | `C2 16 7A 60`| `C2167A60` ✓ |
| CAL     | 0x06     | 0x080000–0x200000 | `45 85 0B EA`| `45850BEA` ✓ |

Note: ASW1 and ASW2 share the same CRC despite differing in ~1.96 MB of bytes. Both regions end in a 4-byte "trailer fixup" word (`36B1E393` for ASW1, `8F193760` for ASW2) followed by zero padding — this is the standard Bosch convention of choosing trailer bytes so that each section's CRC32 lands on a target constant. This is a property of the binary, not of the algorithm. Our implementation just computes `zlib.crc32` of the slice and feeds it in.

Successful response: `71 01 02 02 00` (status = 0x00).

### 2.5 Section → block-ID table (confirmed for 4K0907557G_0003)

Order in the table matches the order MM writes them during a full flash:

| MM order | Block ID | Physical addr | Plaintext size | Section name (this project) |
|----------|----------|---------------|----------------|------------------------------|
| 1        | `0x02`   | 0x80200000    | 0x00200000 (2 MB)   | ASW1 |
| 2        | `0x03`   | 0x80400000    | 0x00200000 (2 MB)   | ASW2 |
| 3        | `0x04`   | 0x80630000    | 0x001D0000 (1.875 MB)| ASW3 |
| 4        | `0x05`   | 0x8001C000    | 0x00044000 (272 KB) | CBOOT |
| 5        | `0x06`   | 0x80080000    | 0x00180000 (1.5 MB) | CAL (maps) |

Implied (not exercised in these captures):
- Block `0x01` is presumably SBOOT (`0x80000000`–`0x8001C000`, 112 KB). MM never erases or writes it. Likely write-protected by the SBOOT itself.
- Block `0x00` and blocks ≥ `0x07` not observed.

**Maps-only flash writes block `0x06` only.** Full flash writes blocks `0x02`–`0x06` in that exact order.

There are two "holes" in the binary that no MM section covers:
- 0x80060000–0x80080000 (128 KB) — gap between CBOOT and CAL
- 0x80600000–0x80630000 (192 KB) — gap between ASW2 and ASW3

These gaps almost certainly contain SBOOT keys / OTP / variant headers and are not writable through the application-level UDS protocol.

### 2.6 Final commit and exit

After the last section's CheckMemory returns success:

1. `31 01 FF 01` — RoutineControl Start, **RID 0xFF01 = CheckProgrammingDependencies**. No params. ECU responds `7F 31 78` once (~0.3–0.4 s), then `71 01 FF 01 00`. This is the global "all sections are consistent, commit signature-checked" routine. If any section's plaintext is inconsistent (e.g. ASW expects a CAL version that wasn't written, or vice-versa), this routine fails and the ECU stays in bootloader.
2. `11 01` — ECUReset hard. ECU acks `51 01` and re-enumerates.
3. After reset: `10 03` extended session → `14 FF FF FF` ClearDTC → tester-present keepalive resumes.

The MM console reports "Writing successful" after `71 01 FF 01 00`; total reported time includes the post-reset ClearDTC.

### 2.7 Timings worth knowing

| Step | Observed wall time |
|------|---------------------|
| Programming Preconditions routine (`0x0203`) | ~3.6 s |
| Programming session response after `10 02` | ~1 s (two `7F 10 78` cycles) |
| ECUReset cycle (`11 01` → enumeration → `51 01`) | ~0.7 s |
| EraseMemory per section | 0.3 s (CBOOT, 272 KB) to 0.9 s (ASW1, 2 MB) |
| RequestTransferExit `37` → `77 00` | ~1.3 s (consistent across sections) |
| CheckMemory `0x0202` | ~2.4 s on ASW3, ~0.4 s on smaller sections |
| CheckProgrammingDependencies `0xFF01` | ~0.3–0.4 s |
| Full "Write Maps" (1 section) | 47 s wall clock |
| Full "Write Maps & Code" (5 sections) | 5 min 51 s wall clock |

P2 = 50 ms, P2* = 5000 ms (from `50 02 00 32 01 F4`). The firmware-side timer for "negative-response-pending timeout" must therefore be ≥ 5 s.

---

## 3. Uncertain / unverified / open questions

### 3.1 Does the ECU stage writes in RAM, or commit to flash per-block?

Evidence in favor of "stage in RAM, flush on TransferExit":
- EraseMemory completes in <1 s even for the 2 MB ASW1 section.
- TransferData ack `02 76 BC` comes back ~5 ms after the last ConsecutiveFrame — far too fast for a flash sector commit.
- `37` RequestTransferExit takes ~1.3 s, suggesting it does the actual flash commit.

Evidence against (or unresolved):
- 2 MB of staged plaintext is a lot of RAM for an MDG1 (typically ~256 KB SRAM). The bootloader may use a sliding buffer that flushes every N KB.
- We don't know what happens if power is lost mid-section. MM's reliability suggests the ECU has crash-recovery in either case.

**Implication for our firmware:** Treat TransferExit as a potentially long-running operation (≥ 5 s timeout) and do not start the next section until the positive `77 00` arrives.

### 3.2 What exactly is encryptionMethod 0xA?

Confirmed:
- The data on the wire after `34 2A …` is not plaintext (visual inspection — it looks high-entropy).
- The compression ratio (1.5 MB → ~262 KB on the wire) is consistent with LZ-style compression *followed by* a same-length encryption pass.
- `firmware/src/flash/sa2_vm.{c,h}` and `firmware/src/flash/lzrb.{c,h}` are staged in the repo (untracked), suggesting Sean already has reference implementations.

Not confirmed from these captures alone:
- The encryption algorithm. Bosch's "0xA" is *usually* AES-128-CBC with a per-VIN or per-box-code key derived from a master key stored in SBOOT, but other Bosch documents describe it as a custom rolling XOR with a 16-byte key. Need to look at `sa2_vm.c` / `lzrb.c` or the `secrets/` folder (not opened) to confirm.
- The IV / nonce. If AES-CBC, what is the IV? Likely either a fixed constant per block ID or derived from the block sequence counter.
- The key. Per CLAUDE.md, AES keys live in `secrets/` (gitignored). I have not opened that directory.

**Validation test once you have a candidate decrypt/decompress implementation:** Take the FirstFrame of the CAL TransferData (line 131 of `mm_MAPS_upload.log`: `1F FF 36 01 4B 7C D6 8D …`) plus the next 584 ConsecutiveFrames, strip the ISO-TP overhead, run through your AES-decrypt → LZRB-decompress pipeline, and compare to the first 4093 plaintext bytes of CAL (file offset `0x80000` of the binary). If they match byte-for-byte, both crypto and compression are validated end-to-end.

### 3.3 What does the `01 … 00` framing on EraseMemory mean?

`31 01 FF 00 01 <BID> 00` — the `01` before the block ID and the trailing `00` are consistent across all 5 sections in the full flash. Most likely interpretation:
- `01` = "number of memory ranges to erase" (= 1 per call).
- `<BID>` = single 1-byte address.
- `00` = padding inside the 6-byte PCI payload (since UDS allows trailing bytes up to the PCI length, which is 6).

But it's also possible `01` is a routine-specific sub-action code, and `<BID> 00` is a 2-byte block-id field where the high byte is always 0 for this ECU. Both interpretations produce the same wire bytes and the same observed behavior. Our firmware should send the bytes exactly as captured.

### 3.4 What if MM's tool issues additional reads between sections that we haven't decoded?

Between section CheckMemory and the next EraseMemory, there are short bursts of traffic in the full flash log that I haven't deeply parsed (lines around 154,278–154,290, 310,269–310,285, 472,752–472,766, 473,990–474,003). Spot-checking, these are `0x37` TransferExit + `0x31 01 02 02 …` CheckMemory + `0x31 01 FF 00 …` next-section Erase — i.e. the end of one section bleeding into the start of the next, no extra reads. But I have not byte-by-byte verified every transition. If you find a mid-flash hiccup in real life, this is the first place to look.

### 3.5 Other ECU types this protocol may or may not apply to

This capture is **MDG1 only** (engine ECU, box `4K0907557G`). The same protocol skeleton is documented to work for:
- MG1 / MD1 / MG1CS family — likely identical (per `hw_reference/MG1_MDG1_Flashing_Research_Part1.md`).
- MED17 family — uses RID 0xFF00 + 0xFF01 but historically with 4-byte physical addresses, not 1-byte block IDs. **Do not assume the block-ID scheme generalizes.**

The `docs/boxcode_database.md` matrix needs a column for "addressing scheme = block-ID vs physical" before Phase 2 firmware can dispatch correctly across ECU families.

---

## 4. Implementation gap analysis — Phase 2 firmware

Status of each protocol element in `firmware/src/flash/mdg1_flash.{c,h}` and adjacent files (untracked: `lzrb.c/h`, `sa2_vm.c/h`):

| Element | Required behavior | Action |
|---------|--------------------|--------|
| Preflight session entry | `10 03` → ID reads → `31 01 02 03` → `14 FF FF FF` (ignore NRC) → `10 02` → `11 01`, twice | Verify state machine in `state_machine/` covers this; today's `mdg1_flash.c` may assume a simpler entry |
| Programming session timeout | P2*=5000 ms; firmware must tolerate `7F 78` for ≥ 5 s | Confirm in ISO-TP coordinator |
| SecurityAccess | `27 11` → seed, `27 12` ← key. SA2 bytecode VM. | Validate `sa2_vm.c` with both captured (seed, key) vectors |
| Fingerprint write `2E F15A` | 9-byte payload, before erase | Add to flash writer; payload format = `<YY YY MM DD>` style, but exact layout TBD — copy MM's value `21 11 22 00 06 46 22 0A 68` as a baseline and probe |
| EraseMemory `31 01 FF 00 01 <BID> 00` | 1-byte block ID, not physical address | **Update if `mdg1_flash.c` currently uses physical addrs** |
| RequestDownload `34 2A 31 <BID> <size3>` | dataFormat=0x2A, ALFID=0x31 (size=3 B, addr=1 B) | **Update if currently using ALFID with 4-byte addr** |
| Max block length | 0x0FFF (use ECU's reported value verbatim; don't hardcode) | Confirm |
| TransferData | block counter mod-256, resets per section | Standard, confirm |
| TransferExit `37` | Long timeout (~1.3 s typical; allow 5 s) | Confirm |
| CheckMemory `31 01 02 02 <CRC32>` | zlib.crc32 of plaintext slice | Add per-section CRC computation; bake the 5 golden CRCs into a unit test for `4K0907557G_0003` |
| CheckProgrammingDependencies `31 01 FF 01` | No params, runs once at end | Add |
| ECUReset `11 01` | After successful flash | Confirm |
| LZRB compression | dataFormat 0x2A high nibble = 2 | Validate `lzrb.c` against round-trip test (see §3.2) |
| AES (encryptionMethod 0xA) | dataFormat 0x2A low nibble = A | Identify algorithm + key + IV; keys in `secrets/` |

**Test vectors available to firmware tests** (all from these captures):
- Seed `C361B058` → Key `F0F2BDD2` (SA2)
- Seed `168BC5E2` → Key `F1CB900F` (SA2)
- Section CRC32s: ASW1=`432DD776`, ASW2=`432DD776`, ASW3=`BD420CC9`, CBOOT=`C2167A60`, CAL=`45850BEA` (for binary `WUAPCBF28NN902533_4K0907557G__0003.bin`)
- Block-ID table (§2.5)
- Full TransferData ciphertext for the CAL section is recoverable from `mm_MAPS_upload.log` lines 129–37582 — useful as a known-ciphertext / known-plaintext pair once decrypt+decompress is implemented

---

## 5. References inside this repo

- `docs/CAN_UDS_PROTOCOL.md` — project-wide UDS service catalog
- `docs/PHASE_2_PREREQUISITES.md` — Phase 2 readiness checklist (needs updating per §2.4.2)
- `hw_reference/MG1_MDG1_Flashing_Research_Part1.md`, `Part2.md` — prior research
- `firmware/src/flash/mdg1_flash.{c,h}` — Phase 2 writer (in progress)
- `firmware/src/flash/sa2_vm.{c,h}` — SA2 seed-to-key VM (untracked)
- `firmware/src/flash/lzrb.{c,h}` — LZRB decompressor (untracked)
- `firmware/test/sa2/`, `firmware/test/lzrb/`, `firmware/test/test_sa2.c`, `firmware/test/test_lzrb.c` — unit-test scaffolds (untracked)

---

*Last updated: 2026-05-10 from captures recorded the same day.*
