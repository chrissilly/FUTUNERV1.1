# ECU AES-128 key fingerprint corpus — 2026-05-12

## What this folder is

A point-in-time inventory of AES-128 key fingerprints found in
Bosch ECU bootloader bins across the SanDisk 034 archive
(`~/034_local/`, 123 GB, 3,521 `.bin` files).

The corpus is mostly **pre-MG1 generation Bosch ECUs** —
MED9 / MED17 / MEx17 / EDC17 + Bosch TCU variants — plus a small
number of MG1 variants (CS001 Flexray, CS002 Autotuner, MG1
CAN-log artifacts) whose keys are NOT the well-known MG1CS002
master. Hence the folder name doesn't claim "pre-MG1 only" — it's
"any ECU key fingerprint outside our current 6-entry
`AES_KEYS_MASTER.md` table."

## Why it exists

`hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md`
established that every Bosch ECU stores its own AES-128 key at one
of two documented offsets in its bootloader flash dump:

| Offset | Family convention |
|---|---|
| `0x18200` | Plain MG1CS002 + most older Bosch generations (MED17, MEx17, EDC17, MEx9, TCU) |
| `0x600200` | MG1CS002IFX (integrated firmware) variants |

A 16-byte block at the right offset, passing entropy + repeated-byte
acceptance, IS the key. We can identify (offset, fingerprint) for
any bin in the archive in milliseconds; that's the inventory in
`key_fingerprint_inventory.{json,md}`.

## Headline numbers

- **3,521 bins** scanned
- **3,059 bins** classifiable (one offset acceptable per the entropy
  filter)
- **53 unique fingerprints** across that classifiable set
- **5 already known** (in `secrets/AES_KEYS_MASTER.md`): MG1 generic,
  MG1CS002, MG1CS011, MD1CP004 T1+T2, MD1CP014
- **48 NEW fingerprints** with sample bin paths + inferred families

## Files in this folder

| File | What it is |
|---|---|
| `README.md` | This file. |
| `key_fingerprint_inventory.json` | Machine-readable. Per-fingerprint: offset, bin count, sample bin paths, inferred ECU family, known/new status. |
| `key_fingerprint_inventory.md` | Human-readable inventory with full fingerprint table + grouping by ECU family + "how to recover key bytes" recipe. |

## What's NOT here

- **No raw key bytes.** Per Hard Rule 5, key bytes live only in
  `secrets/` (gitignored). This corpus stores fingerprints + offsets
  + paths to bin files. Anyone with read access to `~/034_local/`
  can recover the raw bytes mechanically (read 16 bytes at the
  listed offset of the sample path); the corpus tells you where to
  look but not the bytes themselves.
- **No boxcode → key assignment table.** That's the *next* step:
  once a NEW fingerprint is studied + its key recovered, a row gets
  added to `secrets/AES_KEYS_MASTER.md` and the per-boxcode
  assignment lands in `secrets/aes_keys_per_boxcode.json`. The
  corpus is feedstock for that work, not the work itself.
- **No production firmware support.** Just because a key is known
  doesn't mean we can flash that ECU class — that needs the full
  per-variant work: SA2 bytecode, flash section map, CRC algorithm,
  post-commit dependencies (see `docs/PHASE_2_PREREQUISITES.md`).

## How to use

To prioritize which NEW fingerprint to chase next, sort by:

- **Bin count** in the inventory — high bin count means many cars
  share that key class; one key unlocks the most ECUs.
- **Inferred family** — match against the boxcodes you want to
  support. E.g. for diesel customers, prioritize `edc17` family.
- **Path-context plausibility** — bins under `Bosch/Motronic/MEx17/UDS/...`
  are higher-confidence real bootloaders than those under
  `*_CANlog_parsed/*` (sniff outputs, possibly artifact-y).

## Provenance

- **Script:** `tools/hermes_sweep.py` (the `--dry-run` path covers
  the pre-scan logic that generated this corpus).
- **Source archive:** `/Volumes/Extreme SSD/GitHub/VAG MDG1/034/`
  (rsynced to `~/034_local/` at 123 GB) — this is the SanDisk
  archive of Bosch ECU dumps + tooling that you connect when you
  want to do work outside the FUTV1.1 tree. The archive itself
  is outside the repo by Hard Rule 5; this corpus is the
  metadata-only summary that lives in the repo for future
  reference.
- **Methodology:** for each bin, read 16 bytes at offsets `0x18200`
  and `0x600200`, compute Shannon entropy + check for repeated-byte
  / all-0xFF / all-0x00 patterns. Accept the offset whose block
  passes both filters. SHA-256 first 4 bytes (= 8 hex chars) is the
  fingerprint. Cross-reference against the 6 known keys' SHA-256
  fingerprints (`251194eb`, `f57f3534`, `29fd0bf7`, `a65a5bb0`,
  `51866eb2`, `51ffa96a`) to classify each as KNOWN or NEW.

## Why this folder is dated

`ecu_key_corpus_2026-05-12/` — the archive shifts over time
(more bins added, dumps refreshed). Successive inventories date
themselves so we can diff "what's new in the corpus" without
overwriting prior snapshots. A future `ecu_key_corpus_<later-date>/`
folder can supersede this one.
