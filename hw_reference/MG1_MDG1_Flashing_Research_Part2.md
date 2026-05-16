# MG1 / MDG1 ECU Flashing Research — Part 2: Cryptographic Chain & Variant Manifest

> Continuation of `MG1_MDG1_Flashing_Research_Part1.md`. Part 1 catalogued
> the file types, flash memory layout, and per-variant exploit-settings
> structure. Part 2 covers what's needed to actually run the flash chain:
> the SA2 security-access algorithm, the per-variant SA2 bytecode + ALFID
> + flash-block-CRC manifest, and the AES-128-CBC payload encryption.
>
> **Status:** SA2 VM implemented and verified against the spec test
> vectors. Per-variant manifest generated. CRC port verified against
> reference. Phase 2 transport stack still pending.
>
> **Last updated:** 2026-05-10

---

## 0. TL;DR

```
seed (4 B from ECU, 0x67 01 response)
       │
       ▼
┌──────────────────────────────────┐
│ SA2 VM — sa2_run(seed, script)   │   per-variant SA2 bytecode (3 distinct
│ firmware/src/flash/sa2_vm.c      │   scripts across MG1 family today)
└──────────────┬───────────────────┘
               ▼
            key (4 B)  ──►  UDS 0x27 02 to ECU
                                   │
                                   ▼
                       ECU grants programming session
                                   │
                                   ▼
   for each block in {ASW0, ASW1, ASW2, DS0}:
       0x31 01 FF 00 …           erase block (RoutineControl)
       0x34 44 <addr> <size>     RequestDownload
       0x36 nn …                 TransferData (AES-128-CBC + LZRB)
       0x37                      RequestTransferExit
       0x31 01 02 02 …           verify per-block CRC32
   0x31 01 FF 01 …               verify dependencies
   0x11 01                       ECUReset
```

The three crypto primitives the dongle has to implement:

1. **SA2 mini-instruction-set VM** — done (`sa2_vm.c`, host-tested).
2. **AES-128-CBC** for `TransferData` payloads — already wired through mbedTLS in `mdg1_flash.c`.
3. **CRC32 + ADD8/16/32 over flash blocks** — done (`mdg1_crc.c`, port verified).

What's missing to flash a real ECU:

- **CAN driver / ISO-TP / UDS service-layer** plumbing — `mdg1_flash.c` calls a `uds_send` callback that no production code yet provides.
- ~~**LZRB codec**~~ — done 2026-05-10. `firmware/src/flash/lzrb.{c,h}`, host-tested 10/10, ground-truth-validated against a real Bosch `.enc` sample.
- **MG1 RSA pubkeys** — not present in any ODX or `exploitsettings.config` we have. Open question whether the exploit chain needs them or pivots around an unsigned-data-block path.
- **Per-variant manifest → runtime resolver** — the manifest exists at `secrets/mdg1_variant_manifest.json`, no code reads it yet.

---

## 1. SA2 — Seed&Key Mini-Instruction-Set VM

### 1.1 What it is

VW80126 Anhang `SA2-060331-V10` defines a tiny stack-less interpreter that
the ECU bootloader and the programming tool both execute. The bootloader
sends a 32-bit pseudorandom seed in response to UDS `0x27 01`; the tool
runs the per-variant SA2 bytecode over that seed and replies `0x27 02
<key>`. The bootloader runs the same script against the same seed, and if
its key matches, programming is unlocked.

A trivial transformation (e.g. `key = seed + constant`) is explicitly
forbidden by VWSA2-1; the script must contain at minimum a 5-iteration
FOR loop with EOR/RSL/RSR/ADD/SUB and a BCC inside.

### 1.2 Instruction set (10 opcodes)

| Hex  | Mnemonic | Operand | Effect on operand                                   | Effect on carry            |
| ---- | -------- | ------- | --------------------------------------------------- | -------------------------- |
| 0x81 | RSL      | —       | rotate left 1 bit (bit 31 wraps to bit 0)           | carry := old bit 31        |
| 0x82 | RSR      | —       | rotate right 1 bit (bit 0 wraps to bit 31)          | carry := old bit 0         |
| 0x93 | ADD ww   | 4 B BE  | operand += value                                    | carry := overflow past 32b |
| 0x84 | SUB ww   | 4 B BE  | operand -= value                                    | carry := value > operand   |
| 0x87 | EOR ww   | 4 B BE  | operand ^= value                                    | carry := 0                 |
| 0x68 | FOR n    | 1 B     | begin loop, n iterations (n=0 illegal)              | unchanged                  |
| 0x49 | NEXT     | —       | end loop body, branch back if remaining > 0         | unchanged                  |
| 0x4A | BCC w    | 1 B     | if carry == 0, PC += w (forward only)               | unchanged                  |
| 0x6B | BRA w    | 1 B     | always: PC += w (forward only)                      | unchanged                  |
| 0x4C | FINISH   | —       | terminate; operand is the key                       | —                          |

Initial state per spec: `operand = seed`, `carry = 0`.

### 1.3 Spec test vectors (Tabelle 4)

Script: `0x68 0x05 0x81 0x4A 0x05 0x87 0x0A 0x22 0x12 0x89 0x49 0x4C`

| Seed       | Key        |
| ---------- | ---------- |
| 0x107778ED | 0x1AAB38B0 |
| 0xAC13491B | 0x02E25348 |
| 0x198F23CE | 0x2F824E58 |
| 0xFA9E0138 | 0x961FE678 |
| 0x27B3EA04 | 0xDEF50AA0 |

All five pairs are verified by `firmware/test/test_sa2.c` against
`firmware/src/flash/sa2_vm.c`.

### 1.4 In-tree implementation

`firmware/src/flash/sa2_vm.{h,c}` — pure C, no IDF/FreeRTOS dependencies,
host-buildable.

Robustness (per VWSA2-7..10):

- Invalid opcodes → `SA2_ERR_INVALID_OPCODE`
- Truncated operands → `SA2_ERR_TRUNCATED`
- BCC/BRA target outside script → `SA2_ERR_INVALID_JUMP`
- FOR with count 0 → `SA2_ERR_LOOP_ZERO`
- NEXT without matching FOR → `SA2_ERR_NEXT_WITHOUT_FOR`
- Loop nesting > 8 → `SA2_ERR_LOOP_OVERFLOW`
- Running off the end → `SA2_ERR_NO_FINISH`
- Hard ceiling of 1 000 000 instructions → `SA2_ERR_BUDGET_EXCEEDED`

Host test runs from `firmware/test/sa2/` via `make run`. 16 cases, all
green: 5 spec test pairs + 6 error-path tests + 4 single-opcode behavioural
tests + 1 smoke test that the in-tree MG1 generic script executes without
VM error.

### 1.5 How `mdg1_flash.c` uses it

```
ctx_init() ──► set_aes_key() ──► set_sa2_script()  ◄── per-variant script
                                       │                from manifest
                                       ▼
                            flash_execute()
                                       │
                                       ▼
                  security_access_seed()  →  UDS 0x27 01, captures seed
                                       │
                                       ▼
                   security_access_key()  →  sa2_run(seed, ctx->sa2_script)
                                       │              │
                                       ▼              ▼
                                 sends 0x27 02 <key>  err if SA2 fails
```

If `set_sa2_script()` was never called, `security_access_key()` returns
`ESP_ERR_INVALID_STATE` rather than guessing.

---

## 2. Per-variant manifest

### 2.1 File

`secrets/mdg1_variant_manifest.json` — generated by
`tools/extract_mdg1_variant_manifest.py`. Lives under `secrets/` because
the per-variant SA2 + flash block map is proprietary IP (Hard Rule 5).

### 2.2 Generator

The script walks two input sources:

1. `hw_reference/vag_mdg1_drive_pull/mg1_full_tree/MG1/**/*.odx` —
   28 unique production-grade ODX files lifted off the 2 TB SSD.
2. `hw_reference/vag_mdg1_drive_pull/exploitsettings/*.config` — 11 files
   listing covered boxcodes and (in one case) the full flash block map.

For each ODX it pulls:

- `<SECURITY-METHOD>SA2</SECURITY-METHOD>` → `<FW-SIGNATURE>` → SA2 hex
- `<SECURITY-METHOD>ALFID</SECURITY-METHOD>` → `<FW-SIGNATURE>` → ALFID hex
- `<SECURITY-METHOD>CRC</SECURITY-METHOD>` × N → per-`DB_FD_NN` block CRC
- `<DATABLOCK>` → segment compressed/uncompressed sizes and FD_NN tag

For each `exploitsettings.config` it pulls the boxcode list and (when
present) the `#FlashTags` block: `version, FD_NN, address, length, name`.

It also runs every extracted SA2 script through a Python reimplementation
of the VM's static decode pass (same opcode table as `sa2_vm.c`) — every
script in the manifest carries `static_check_ok: true` plus a detail
string for the audit trail.

### 2.3 Schema (top level)

```jsonc
{
  "schema_version": "1.0",
  "generated_at":   "2026-05-10",
  "summary": {
    "odx_files_parsed":      28,
    "exploitsettings_parsed": 11,
    "unique_sa2_scripts":      3,
    "variants_known":         12,
    "boxcodes_indexed":       13
  },
  "sa2_scripts": [           // dedup'd across variants
    {
      "hex":              "...62 hex chars...",
      "byte_length":      31,
      "used_by_variants": ["MG1 CS001"],
      "alfids":           ["0131"],
      "static_check_ok":  true,
      "static_check_detail": "ok (FINISH reached)"
    }, ...
  ],
  "variants": {
    "MG1 CS002": {
      "covered_boxcodes":  ["8W0907559H 0009", "8W0907559G 0011", "80A907559C 0005"],
      "sa2_scripts_hex":   ["..."],
      "alfids":            ["0131"],
      "block_map": [        // populated only when #FlashTags is in the .config
        {"version":"1.18","fd_tag":"FD_01","address":"0x08FFF800","length":"0xD8","name":"FlashRecord_Cboot"},
        {"version":"1.18","fd_tag":"FD_01","address":"0x08FFFFF8","length":"0x4", "name":"Checksum"},
        ...
      ],
      "odx_files":         ["hw_reference/.../FL_8W0907559H_0005__V001.odx"]
    }, ...
  },
  "boxcode_index": {        // boxcode → variant family
    "8V0907115C 0002": "MG1 CS001",
    "8W0907559H 0009": "MG1 CS002",
    ...
  }
}
```

### 2.4 Findings

Three distinct SA2 scripts are in use across the MG1 family today, not one
"generic" — the in-tree label was misleading:

| Variant family                                 | SA2 (hex, abbrev)          | ALFID |
| ---------------------------------------------- | -------------------------- | ----- |
| `MG1 CS001`                                    | `680787010720…17494C4C`    | 0131  |
| `MG1 CS002` (8W0907559H S0005)                 | `680787310720…17494C4C`    | 0131  |
| `MG1 CS002IFX` / `MG1 CS002IFX RS` / Tests     | `680787040120…18494C4C`    | 0131  |

All three pass static decode — bytecode well-formed, opcodes valid,
operands present, FINISH reached. Runtime correctness will need a real
ECU bench (ECU sends a seed; we compare key against ECU's expectation).

**Flash block map** is only populated for `MG1 CS002`:

| FD     | Address       | Length | Name              |
| ------ | ------------- | ------ | ----------------- |
| FD_01  | `0x08FFF800`  | 0xD8   | FlashRecord_Cboot |
| FD_01  | `0x08FFFFF8`  | 0x4    | Checksum          |
| FD_02  | `0x0933F800`  | 0xD8   | FlashRecord_ASW0  |
| FD_02  | `0x0933FFF8`  | 0x4    | Checksum          |
| FD_03  | `0x095FF800`  | 0xD8   | FlashRecord_ASW1  |
| FD_03  | `0x095FFFF8`  | 0x4    | Checksum          |
| FD_04  | `0x08FCF800`  | 0xD8   | FlashRecord_ASW2  |
| FD_04  | `0x08FCFFF8`  | 0x4    | Checksum          |
| FD_05  | `0x0977F800`  | 0xD8   | FlashRecord_DS0   |
| FD_05  | `0x0977FFF8`  | 0x4    | Checksum          |

This map appears once and is shared across the CS002 family — the other
variant configs only carry boxcode lists, not block maps. Probably means
the Bosch CS002 platform uses a fixed memory map; the other variants
either reuse it or Bosch versions it elsewhere we haven't found yet.

13 boxcodes are now indexed → variant family in the manifest.

---

## 3. CRC port verification

`firmware/src/flash/mdg1_crc.c` was a port of Aftab Hussain's
`MDG1_CRC.cpp` (chiptuningshop). Diffed line-by-line against the
reference now living at
`hw_reference/vag_mdg1_drive_pull/checksum/MDG1_CRC.cpp`:

| Property                                     | Reference   | In-tree port | Match |
| -------------------------------------------- | ----------- | ------------ | ----- |
| CRC32 polynomial                             | 0xEDB88320  | same         | ✓     |
| CRC32 init                                   | 0xFFFFFFFF  | same         | ✓     |
| CRC32 xorout                                 | `~crc`      | same         | ✓     |
| ADD{8,16,32} init                            | 0xFFFFFFFF  | same         | ✓     |
| DEADBEEF marker scan in both endiannesses    | yes         | yes          | ✓     |
| Header start offset from marker              | +0x100      | same         | ✓     |
| `num_entries` byte offset within header      | +0x13       | same         | ✓     |
| `address_fix` dword offset within header     | +0x50       | same         | ✓     |
| Per-entry layout (start@0, end@4, algo@11, value@12, stride 16) | same | same | ✓     |
| Algorithm IDs (1=ADD32, 2=CRC32, 8=ADD8, 16=ADD16) | same  | same         | ✓     |

Differences (intentional, not bugs):

- Port adds bounds checks + a 64-entry-per-block cap (defends against
  malicious inputs; reference would crash).
- Port splits validate-only and fix modes (`mdg1_crc_validate` /
  `mdg1_crc_fix`); reference always fixes.
- **Reference does up to 3 fix-and-reverify passes** to handle nested
  CRC coverage; **port does a single pass.** Not a correctness bug for
  typical ASW0/1/2/DS0 layouts but worth a follow-up if a FlashRecord
  is ever encountered with a CRC over a region that itself contains
  another CRC.

---

## 4. Asset inventory (this session)

Pulled to `hw_reference/vag_mdg1_drive_pull/` (~2.5 GB total). Full
breakdown in that folder's `NOTES.md`. Highlights:

| Folder                  | What it gave us                                                      |
| ----------------------- | -------------------------------------------------------------------- |
| `sa2_spec/`             | `SA2-060331-V10.pdf` (basis for the VM); plus extracted `.txt`       |
| `mg1_full_tree/MG1/`    | 28 production ODX files + 11 exploitsettings; source for §2          |
| `test_odx/`             | 17 `_TEST_EXPLOIT.odx` (subset of the above; kept for completeness)  |
| `exploitsettings/`      | 11 per-variant configs (1 with full block map)                       |
| `checksum/`             | `MDG1_CRC.cpp` reference (basis for §3 diff)                         |
| `packing/lzrb_src/`     | Java LZRB codec — needs C porting before flash payloads can be built |
| `protocol_docs/`        | 1.5 GB Bosch flashing/UDS/KWP/FDS/VW80126 documentation              |
| `re_manuals/`           | Tricore TC1766/1798/29x, AUDO-NG bootloader, CCP, UDS 2013           |
| `rsa_pubkeys/`          | 14 RSAPublicKeys JSONs — **EDC17/MEx17 only, none for MG1**          |

Files that contain inline keys (`RL_MDG1.cpp`, `*.enc`) were relocated
to `secrets/drive_pull_keyed_sources/` per Hard Rule 5 immediately on
pull.

---

## 5. What's still missing

### 5.1 MG1 RSA public keys

Not present in any ODX, not in any `exploitsettings.config`, not in any
file under `vag_mdg1_drive_pull/`. The only RSAPublicKeys JSONs we have
are for the older EDC17/MEx17 platforms (TP2 era).

Two hypotheses to confirm before Phase 2 wiring:

1. The exploit chain pivots around an unsigned-data-block path on MG1 —
   no RSA needed at runtime. Confirmable by tracing one production
   flash session (no `full_magic_flash_*.log` capture is on the drive
   either) or by the SEFI flasher source (skipped per direction).
2. The MG1 RSA pubkeys live in a place we haven't pulled — most likely
   inside the SEFI flasher binary or an internal Audi key repo. Check
   the SEFI source if/when that constraint is lifted.

A live MagicMotorsport sniff resolves this empirically — Phase 3.5 of
`docs/CLAUDE_CODE_CAPTURE_FLASH.md` scans every TransferData payload
for high-entropy ≥256 B windows (RSA-modulus-shape) and reports the
count. Zero hits across all 5 cycles is the falsifying evidence for
hypothesis 1.

### 5.2 LZRB codec — DONE 2026-05-10

Both directions ported to C. Pure-C implementation (no IDF / mbedTLS / FreeRTOS
dependencies) at `firmware/src/flash/lzrb.{h,c}`, ~470 lines including the
sliding-window machinery and pattern-fill optimisation for repeating
1/2/4-byte regions. Host test at `firmware/test/lzrb/` runs 10
round-trip cases (empty, 1B, all-zeros, all-0xFF, repeating 4-byte
pattern, random LCG, 64 KB mixed, repeated English phrase) — all green.

**Wire-format ground-truth check passed.** The encrypted Bosch sample
`secrets/drive_pull_keyed_sources/EMEM_8W0907559H_0005__V001.FD_05FLASHDATA.enc`
(184080 B) was AES-128-CBC-decrypted with the MG1CS002 key and IV from
`secrets/AES_KEYS_MASTER.md`, then run through `lzrb_decompress` with
`expected_out_len = 1572864` (the ODX `UNCOMPRESSED-SIZE` for DB_05).
Result: 1,572,864 bytes out, status OK, output starts with `0xDEADBEEF`
(the marker `mdg1_crc.c` scans for) at offset 0 — exactly where the
manifest's `FlashRecord_DS0` placement says it should be. That's
empirical proof the codec is wire-compatible with Bosch's production
toolchain, stronger than a Java cross-validation would have produced.

**Confirmed flash chain ordering**: `LZRB-compress(plaintext) →
AES-128-CBC-encrypt → .enc on disk → ECU receives → AES-decrypt →
LZRB-decompress → flash`. Encrypt is the outer layer.

### 5.3 Production flash log

No `full_magic_flash_s5_frfupdate.log` (or equivalent) was on the drive.
A real capture is the cleanest way to validate the SA2/AES/CRC/LZRB chain
end-to-end against an authoritative session.

### 5.4 SA2 runtime ground truth

Static decode passes for all three scripts. Runtime correctness needs at
least one captured `(seed, key)` pair from a real ECU per variant. That's
a bench task; no pure-software substitute.

The capture plan that resolves this — alongside §5.1 (RSA question) and
the open RoutineControl byte formats — is `docs/CLAUDE_CODE_CAPTURE_FLASH.md`,
tracked as P-01 in `docs/PHASE_2_PREREQUISITES.md`. Phase 3.5 of that
plan (added 2026-05-10) extracts the `(seed, key)` pair from each of
five MagicMotorsport flash cycles and writes them as runtime test
vectors against `sa2_vm.c`.

### 5.5 Transport stack

`mdg1_flash.c` expects a `uds_send` callback. No production CAN driver /
ISO-TP / UDS service-layer wiring connects to it yet. Until that's in
place, the flash chain is research-grade only.

### 5.6 Build integration

`firmware/src/flash/{sa2_vm,mdg1_flash,mdg1_crc}.c` are not yet listed in
`firmware/src/CMakeLists.txt`. Add them when Phase 2 work begins. Not
done preemptively because their callers (transport stack, manifest
resolver) don't exist yet and adding orphaned code to the build invites
warning churn.

---

## 6. Code map (relative to `FUTV1.1/`)

| File                                          | Purpose                                                                  |
| --------------------------------------------- | ------------------------------------------------------------------------ |
| `firmware/src/flash/sa2_vm.h`                 | SA2 VM API — `sa2_run(seed, script, len, *key_out)`                      |
| `firmware/src/flash/sa2_vm.c`                 | VM impl, ~150 lines, pure C                                              |
| `firmware/src/flash/mdg1_flash.h`             | Flash context with `sa2_script*` + setter declaration                    |
| `firmware/src/flash/mdg1_flash.c`             | Flash sequencer; calls `sa2_run` from `security_access_key()`            |
| `firmware/src/flash/mdg1_crc.{c,h}`           | DEADBEEF-marker block CRC engine (validate / fix)                        |
| `firmware/src/flash/lzrb.{c,h}`               | LZRB codec (compress + decompress); pure C, no IDF deps                  |
| `firmware/test/test_lzrb.c`                   | 10-test round-trip host harness                                          |
| `firmware/test/lzrb/Makefile`                 | Host build (`make run`)                                                  |
| `firmware/test/test_sa2.c`                    | 16-test host harness (5 spec pairs + error paths + opcode behaviours)    |
| `firmware/test/sa2/Makefile`                  | Host build (`make run`)                                                  |
| `tools/extract_mdg1_variant_manifest.py`      | ODX + exploitsettings → manifest JSON                                    |
| `secrets/mdg1_variant_manifest.json`          | Generated per-variant manifest (gitignored)                              |
| `secrets/AES_KEYS_MASTER.md`                  | AES-128 keys + IVs (existing, unchanged this session)                    |
| `secrets/aes_keys_per_boxcode.json`           | Boxcode → AES key lookup (existing)                                      |
| `secrets/drive_pull_keyed_sources/`           | Key-bearing reference sources moved here from the drive pull             |
| `hw_reference/vag_mdg1_drive_pull/NOTES.md`   | Full asset inventory of the 2.5 GB drive pull                            |
| `hw_reference/vag_mdg1_drive_pull/sa2_spec/`  | SA2 spec PDF + extracted text                                            |

---

## 7. Next-session checklist (when Phase 2 starts)

1. Add `flash/sa2_vm.c`, `flash/mdg1_flash.c`, `flash/mdg1_crc.c`, and
   `flash/lzrb.c` to `firmware/src/CMakeLists.txt`.
2. Write a manifest resolver: given a boxcode, produce
   `{aes_key, aes_iv, sa2_script_ptr, sa2_script_len, block_map[]}`.
   Reads `secrets/mdg1_variant_manifest.json` (probably embedded as a
   const blob at build time) and `secrets/aes_keys_per_boxcode.json`.
3. Wire `uds_send` callback to the existing ISO-TP coordinator — once
   that's done, `mdg1_flash_execute()` becomes a real flash run.
4. ~~Port the LZRB codec from Java to C~~ — done 2026-05-10. Host tests
   pass; ground-truthed against a Bosch `.enc` sample.
5. Settle the RSA question (§5.1) before declaring Phase 2 ready.
6. Capture a ground-truth `(seed, key)` pair on the bench for each of the
   3 known SA2 scripts; add as runtime tests in `test_sa2.c`.
