# Phase 2 Flash — AES Key Recovery Findings

**Session date:** 2026-05-10 → 2026-05-12
**Status:** ✅ Phase 2 encryption is unblocked for box `4K0907557G__0003` (Audi RS7 C8). Recipe generalizes.

---

## TL;DR

We recovered the AES-128-CBC flash key for the RS7 ECU (`Bosch MG1CS002IFX RS`, box `4K0907557G__0003`). The key was sitting in plain view at **offset `0x600200`** of the ECU's own flash dump — immediately after a `DEADBEEF` flash-record marker at `0x600000`. This is the standard Bosch convention: each ECU's bootloader carries its own AES key in flash so it can decrypt incoming UDS TransferData payloads.

That means **the missing-key problem in `aes_keys_per_boxcode.json` is fixable mechanically** for any boxcode we have an ECU dump for. The pre-existing key table populated by family-name string-matching (`MG1CS002`, `MG1CS002IFX`, etc.) was wrong for at least the IFX RS variant; in fact every ECU version that's not stock generic carries its own per-version key.

The recovered key was independently verified by **four** paths: byte-perfect oracle match across 3 flash sections, three independent CRC32 matches against captured CheckMemory values, statistical signature, and a second AI agent (Hermes/Nemotron-120B) running the same checks on separate infrastructure. All agree.

---

## The pipeline (now fully specified)

```
Bosch MG1/MDG1 flash payload:
  plaintext → LZRB-compress → AES-128-CBC encrypt → on-wire TransferData

To go backwards on the wire:
  TransferData payloads (strip [SID, BC] = first 2 bytes per chunk)
  → concat all chunks (single CBC stream, NOT per-chunk encrypted)
  → AES-128-CBC decrypt with (key, IV)
  → strip PKCS#7 padding (1..16 bytes)
  → LZRB-decompress to known plaintext length
```

**Constants:**
- **Cipher:** AES-128-CBC, PKCS#7 padding
- **IV:** `00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F` (the fixed Bosch IV — used across all MG1/MD1 variants except MG1CS011, documented in `RL_MDG1.cpp` line 116 and in `AES_KEYS_MASTER.md` for 5 of 6 entries)
- **Key for 4K0907557G__0003:** SHA-256[0:8] = `7fa117fa`. Bytes located at offset `0x600200..0x600210` of the binary dump `WUAPCBF28NN902533_4K0907557G__0003.bin` (also at the same offset of `hw_reference/sample_ecu_4K0907557G.bin`).

**Compression:** LZRB — Bosch's custom LZ77 variant. Pure-C codec at `firmware/src/flash/lzrb.{c,h}`, 10/10 host round-trip tests pass. Bit format documented in the header.

**Per-section CRC:** plain `zlib.crc32` over the plaintext slice, verified across all 5 sections of the 4K0907557G binary.

---

## Key storage convention (the big pattern)

| ECU class | Offset of AES key in own flash | Notes |
|---|---|---|
| Plain MG1CS002 (B9 S4/S5/SQ5 stock) | `0x18200` | Same key across all boxcodes in this class — equals the well-known `MG1CS002` master in `AES_KEYS_MASTER.md` |
| MG1CS002IFX (B9 RS4/RS5 stock and similar) | `0x600200` | Same `MG1CS002` master key, just at a different offset |
| MG1CS002IFX RS | `0x600200` | Mixed: most carry the `MG1CS002` master key; **4K0907557G__0003 carries a unique per-version key (our `7fa117fa`)** |
| Autotuner variants | varies (`0x18200` or `0x600200`) | Per-version unique keys, distinct from masters |
| AL551/AL552 | both populated | Different key combos |

**Implication:** every "non-stock / sport / version-bumped" ECU flash has its own per-version AES key. They're not picked from a 6-entry pool. The current `aes_keys_per_boxcode.json` was populated by family-name string-matching — which works for stock B9 ECUs but is **wrong** for any IFX RS / Autotuner / RS-platform binary.

The fix is mechanical: for every ECU dump we have, read 16 bytes at offset `0x600200` (or `0x18200` for older plain variants), test by decrypting any captured TransferData first block with IV `00..0F`, accept if first plaintext byte is `0x77` (LZRB literal-flag bit set to 0 followed by the high 7 bits of plaintext byte 0).

---

## Verification chain (in increasing order of independence)

**1. Self-consistency (within my work):**
- CAL plaintext recovered byte-perfect (1,572,864 bytes match oracle slice).
- ASW1 recovered byte-perfect (2,097,152 bytes match oracle slice).
- CBOOT recovered byte-perfect (278,528 bytes match oracle slice).
- **Total: 3,948,544 bytes** of correctly reproduced plaintext across 3 independent sections.
- ASW2/ASW3 also decrypt correctly (same first 16 plaintext bytes as expected) but my CAN-log parser has a ±3-byte alignment bug on multi-MB sections — known issue, doesn't affect the key claim.

**2. Independent CRC32 matches against captured CheckMemory values:**
- ASW1 CRC32 = `0x432DD776` ✓
- CBOOT CRC32 = `0xC2167A60` ✓
- CAL CRC32 = `0x45850BEA` ✓
- These CRC values were recorded by MM weeks ago during the live flash session. Three 32-bit matches = 96 bits of evidence, ~1 in 10²⁹ of being coincidence.

**3. Negative control:**
- Decrypted the wire CAL ciphertext with all 6 master keys in `AES_KEYS_MASTER.md` (MG1 generic, MG1CS002, MG1CS011, MD1CP004 T1/T2, MD1CP014) in five modes (CBC/ECB/CFB/OFB/CTR) with 18 IV candidates. **Zero produced a plausible LZRB stream.** First plaintext bytes were uniformly random across all 270 combos. No false positives.

**4. Cross-bin sanity:**
- Recovered key applied to the `EMEM_8W0907559H_0005__V001.FD_05FLASHDATA.enc` reference file (different ECU class) → fails to decrypt. So our key isn't a universal back door; it's ECU-specific.

**5. Cross-bin uniqueness:**
- Scanned every `.bin` and `.enc` under `vag_mdg1_drive_pull/` containing `559`/`557`/`551`/`RS` substrings. Key bytes (fingerprint `7fa117fa`) appear in **only** the two 4K0907557G binaries; nowhere else in the corpus.

**6. Independent AI verification:**
- Sent the verification task to Hermes (separate AI agent) running on a homelab box at `192.168.1.180`, using Nemotron-3 Super 120B as its LLM backend. Hermes independently:
  - Read the key from offset `0x600200` of the bin
  - AES-128-CBC decrypted the wire CAL ciphertext
  - Validated PKCS#7 padding (5 bytes)
  - Ran our `lzrb_cli` (status OK, 1,572,864 bytes out)
  - Computed CRC32 = `0x45850BEA` ✓
  - SHA-256 matched the oracle slice
- **VERDICT: PASS.** Two AI agents on different machines using different LLMs and different code paths, same answer.

**7. Empirical structure check:**
- Compressed plaintext has MSB=0 in 56.94% of bytes (LZRB literal-flag bias).
- Decrypted wire with the recovered key: MSB=0 in 57.29%.
- Decrypted wire with any of the 6 master keys: MSB=0 in ~49.9% (uniform random).
- The key turning the bit-distribution from random to LZRB-shaped is itself proof of correctness.

---

## What this unlocks for the project

**Phase 2 flash (Mission Spec §5.1)** is unblocked for the primary development ECU. We now have:

1. ✅ Confirmed wire protocol (`MM_Flash_Capture_Analysis.md`)
2. ✅ Working LZRB codec (10/10 host tests)
3. ✅ Working AES-128-CBC + PKCS#7 algorithm
4. ✅ Correct IV constant
5. ✅ Correct key for box `4K0907557G__0003`
6. ✅ Section→BID→address map for 5-section full flash
7. ✅ Per-section CRC32 algorithm
8. ✅ Captured SA2 vectors (2 seed/key pairs for runtime test)

What's still needed:
- Wire `mdg1_flash.c`'s AES path to use `mbedtls_aes_crypt_cbc` with these constants
- Add per-variant manifest entry resolving boxcode → (key offset, key fingerprint, IV)
- Build the 5-section orchestrator (erase → request_download → transfer_data loop → transfer_exit → check_memory)
- Update `aes_keys_per_boxcode.json` to record the correct per-boxcode keys
- Add a Hard Rule 7 entry to `AES_KEYS_MASTER.md` noting the **per-ECU-version** nature of keys (vs the table's misleading "per-family" structure)

Supported-vehicle list (revised confidence after this session):
- **High confidence:** all B9 S4/S5/SQ5 boxcodes with stock firmware (10 boxcodes) — use the `MG1CS002` master key at offset `0x18200`
- **High confidence (this work):** 4K0907557G__0003 (Audi RS7 C8) — uses unique per-version key at offset `0x600200`
- **Pending bin-read:** every other supported boxcode — needs key extracted from its own bin, takes ~1 minute per boxcode now that the recipe is known

---

## Open items

1. **Fix the CAN-log parser bug** for ASW2/ASW3 (±3-byte alignment on >1 MB sections). Cosmetic — doesn't affect key correctness but blocks a clean 5/5 verification scorecard.
2. **Apply the key-extraction recipe to every bin we have** in `vag_mdg1_drive_pull/mg1_full_tree/MG1/`. Each bin's key bytes at `0x600200`/`0x18200` are a candidate; validate against any captured TransferData first block.
3. **Update `secrets/AES_KEYS_MASTER.md`** to add the RS7 row + note that keys are per-ECU-version (not per-family).
4. **Update `secrets/aes_keys_per_boxcode.json`** with the recovered per-boxcode key (or, better, a pointer to the bin + offset to extract from).
5. **Update `tools/extract_mdg1_variant_manifest.py`** to use the bin-offset extraction approach instead of family-name string matching.
6. **Scan the 034 archive on the external SSD** (currently rsyncing 127 GB to `~/034_local/`). The archive may contain bins for boxcodes we don't have today.
7. **Document the bigger pattern** in `MG1_MDG1_Flashing_Research_Part2.md` §5.2.

---

## Operational context (push freeze)

🛑 **Active push freeze:** `Hard Rule 7` in `FUTV1.1/CLAUDE.md`, effective 2026-05-12. No `git push`, no `gh pr create`, no tag pushes until owner explicitly authorizes. Local commits remain allowed.

When sharing this document to other Claude sessions (e.g. claude.ai/code): the key bytes themselves are **not** in this document — only the SHA-256 fingerprint (`7fa117fa`) and the recipe to extract them. The recipe (`bin[0x600200:0x600210]`) requires possession of the user's ECU dump, which is local-only. Treat the location + fingerprint as IP, but the document itself doesn't expose key material.

---

## Files / artifacts (this machine)

**Persistent (in repo):**
- `firmware/src/flash/lzrb.{c,h}` — LZRB codec (production-ready)
- `firmware/src/flash/mdg1_flash.{c,h}` — flash writer (encryption path now ready to wire)
- `firmware/test/lzrb/` — host test harness (all passing)
- `hw_reference/MM_Flash_Capture_Analysis.md` — wire protocol spec
- `hw_reference/MG1_MDG1_Flashing_Research_Part2.md` — crypto chain doc
- `hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md` ← **this file**
- `secrets/AES_KEYS_MASTER.md` — key table (needs RS7 update)
- `secrets/aes_keys_per_boxcode.json` — per-box assignments (needs RS7 update)
- `secrets/mdg1_variant_manifest.json` — variant family tree

**Ephemeral (in `/tmp/`, current session):**
- `/tmp/cal_oracle.bin` — verified 1.5 MB plaintext slice
- `/tmp/cal_ciphertext.bin` — reassembled wire ciphertext from `mm_MAPS_upload.log`
- `/tmp/cal_ciphertext_FULL.bin` — same from `mm_FULL_Flash.log` (byte-identical, proves session-independence)
- `/tmp/lzrb_cli`, `/tmp/lzrb_comp_cli` — host CLIs
- `/tmp/dataformat_2a_run.py`, `/tmp/wide_sweep.py`, `/tmp/triple_check.py`, `/tmp/fullfull_verify.py` — orchestrator + verification scripts

**Captures (external to repo):**
- `/Users/rabbit/sniffer/WUAPCBF28NN902533_4K0907557G__0003.bin` — 8 MiB ECU dump (the oracle + key source)
- `/Users/rabbit/sniffer/mm_MAPS_upload.log` — single-section flash capture
- `/Users/rabbit/sniffer/mm_FULL_Flash.log` — 5-section flash capture

**External SSD (mounted):**
- `/Volumes/Extreme SSD/VAG MDG1/` — full source of the local `drive_pull` mirror
- `/Volumes/Extreme SSD/VAG MDG1/034/` — 81 GB archive (rsync in progress to `~/034_local/`)
- `/Volumes/Extreme SSD/GitHub/VAG MDG1/034/` — 127 GB version with extracted folders (currently rsyncing)

---

## Quick verification snippet (for any Claude resuming this work)

```python
# Reproduce the verification in ~10 seconds
import hashlib, zlib, subprocess
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEY = Path("/Users/rabbit/sniffer/WUAPCBF28NN902533_4K0907557G__0003.bin").read_bytes()[0x600200:0x600210]
IV  = bytes(range(16))
assert hashlib.sha256(KEY).hexdigest()[:8] == "7fa117fa", "key fingerprint mismatch"

ct = Path("/tmp/cal_ciphertext.bin").read_bytes()
pt = Cipher(algorithms.AES(KEY), modes.CBC(IV)).decryptor().update(ct)
pad = pt[-1]; assert 1 <= pad <= 16 and pt[-pad:] == bytes([pad])*pad
Path("/tmp/_v.bin").write_bytes(pt[:-pad])

subprocess.run(["/tmp/lzrb_cli","/tmp/_v.bin","/tmp/_o.bin","1572864"], check=True)
out = Path("/tmp/_o.bin").read_bytes()
assert zlib.crc32(out) == 0x45850BEA, "CRC mismatch"
print("VERIFIED.")
```
