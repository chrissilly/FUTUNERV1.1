# AES-128 Encryption Keys — MASTER REFERENCE

> **DO NOT COMMIT.** This folder is gitignored.
>
> 🛑 **PUSH FREEZE (active 2026-05-12 → until owner lifts):** even non-secret files in this repo are frozen from `git push`. Local commits are OK; nothing leaves the machine until the owner says so. See Hard Rule 7 in `../CLAUDE.md`.

## Per-ECU Keys

| ECU Variant | Key (hex) | IV (hex) |
|-------------|-----------|----------|
| MG1 generic | C7 12 B1 F1 4B 31 AD C1 FD 33 04 D0 FB D6 DE 6B | 00..0F |
| MG1CS002 | 83 41 C1 ED 72 CD C2 5F 9B AF 7A EA 94 61 77 EF | 00..0F |
| MG1CS011 | 6D A9 5B 9D C4 C2 F9 8B 5C 00 A3 04 A9 6A 1F 96 | 6D C9 5D 2E 09 3A DD 59 10 D1 36 7B 7F F5 A0 2B |
| MD1CP004 Type1 | 41 45 53 2D 44 65 66 61 75 6C 74 2D 4B 65 79 32 | 00..0F |
| MD1CP004 Type2 | 2B D2 A3 53 C3 59 35 D5 E1 7C 80 B8 9E 90 7B 7B | 00..0F |
| MD1CP014 | 0F 78 AE 6A FA 92 23 3B 71 8F 2C 13 85 D3 11 3A | 00..0F |

## Per-Boxcode Mapping
See `aes_keys_per_boxcode.json` (21/36 boxcodes have keys).

## Algorithm
- **Cipher:** AES-128-CBC
- **Compression:** LZRB (Bosch custom LZ-based)
- **CRC:** DEADBEEF marker scan, header CRC32, per-block (ADD8/ADD16/ADD32/CRC32)
- **SA2 bytecode:** 6807870401201593050220164A03826B068193060320178407042018494C4C
