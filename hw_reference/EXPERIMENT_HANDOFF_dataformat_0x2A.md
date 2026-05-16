# Handoff — dataFormat 0x2A decryption experiment

**State:** Phase 0 halted on missing inputs. Hermes binary located but
endpoint not yet probed. Next session should resume from "Open
questions for the user" below.

---

## What we're doing

Crack Bosch MDG1 `dataFormat 0x2A` (LZRB + AES) using the captured
MagicMotorsport CAL TransferData stream. 30-min wall-clock timebox.
Don't commit. Don't modify `firmware/src/`. Full experiment spec in
the user's prompt (paste it back into the new session — see "Original
prompt to re-issue" at the bottom).

Phases:
- **0** Assemble: reassemble CAL ciphertext from MM log, locate
  plaintext oracle, load MG1CS002 AES key.
- **1** Hermes preflight call (≤10 s).
- **2** Trial loop: AES-128-CBC with IV hypotheses zero / block-counter /
  fixed-Bosch (00010203…0F) / Hermes-suggested 4th. Decrypt →
  `lzrb_decompress` → byte-diff against oracle. 25 min hard cap.
- **3** Hermes post-mortem (only if no SUCCESS).
- **4** Report to `/tmp/dataformat_2a_experiment.md` and append to
  `~/esp/obd/file-update-2026-05-10.md`.

---

## Inputs already verified

| Item | Path | Notes |
|------|------|-------|
| Project CLAUDE.md | `~/esp/obd/FUTV1.1/CLAUDE.md` | Read. Hard Rule 5: don't exfiltrate AES keys. |
| Analysis doc | `~/esp/obd/FUTV1.1/hw_reference/MM_Flash_Capture_Analysis.md` | Read. §2.4 wire format; §2.4.5 says CAL = block ID 0x06, file offset 0x80000–0x200000 (1.5 MiB plaintext). |
| LZRB API | `~/esp/obd/FUTV1.1/firmware/src/flash/lzrb.h` | Read. `lzrb_decompress(in, in_len, out, out_cap, expected_out_len, *out_len_actual)` — pure C, no deps. Implementation at `lzrb.c`. |
| Variant manifest | `~/esp/obd/FUTV1.1/secrets/mdg1_variant_manifest.json` | 10,459 B file exists. NOT yet opened (waiting for resume to avoid accidental key disclosure in working memory). Per prior status log, MG1CS002 AES-128 key is the one to use for this binary. |
| AES keys master | `~/esp/obd/FUTV1.1/secrets/AES_KEYS_MASTER.md` + `secrets/aes_keys_per_boxcode.json` | Also under `secrets/`. |
| Flashed binary (oracle) | `/Users/rabbit/sniffer/WUAPCBF28NN902533_4K0907557G__0003.bin` | 8 MiB. CAL region = offset 0x80000..0x200000 inclusive of low / exclusive of high (1.5 MiB = 1,572,864 B). |
| MM captures | `/Users/rabbit/sniffer/mm_MAPS_upload.log` (37,622 lines, CAL-only flash) and `mm_FULL_Flash.log` (511,495 lines, 5-section full flash) | Text logs in MM's own format, NOT candumps. For CAL-only decryption use `mm_MAPS_upload.log` — single section, simpler parse. |

---

## Hermes endpoint — what we've found

- Wrapper: `/Users/rabbit/.local/bin/hermes` (bash, 4 lines).
- Real binary: `/Users/rabbit/.hermes/hermes-agent/venv/bin/hermes`
  (Python venv).
- The wrapper runs `hermes` as a **CLI**, not a server it talks to.
  User says "hermes agent is localhost" — so probably either
  (a) hermes itself is a CLI client that hits a local server, or
  (b) hermes is the server and needs to be running for the OpenAI-compat
  endpoint to answer.
- `lsof -iTCP -sTCP:LISTEN` returned nothing on common LLM ports
  (8080, 8000, 11434, 1234, 5000, etc.) — **server is probably not
  running**. Next session: start hermes-agent first or have user start it.

### Next-session resume steps for Hermes

```bash
# Inspect hermes-agent layout
ls -la /Users/rabbit/.hermes/hermes-agent/
find /Users/rabbit/.hermes/hermes-agent -maxdepth 3 -name "README*" -o -name "config*" -o -name "*.toml" -o -name "*.yaml" 2>/dev/null
/Users/rabbit/.local/bin/hermes --help 2>&1 | head -40
```

Once endpoint is up:

```bash
export HERMES_URL=http://localhost:<PORT>/v1/chat/completions
export HERMES_MODEL=<model name from --help or config>
export HERMES_TOKEN=""   # if no auth
```

---

## Gaps that blocked Phase 0

1. **Hermes env vars** unset — see above; resolve before Phase 1.
2. **`firmware/test/can_capture/fixtures/magicmotorsport/SUMMARY.md`
   does not exist.** The directory has only:
   - `flash_run_1_4K0907557G_0003.candump.empty.143313` (0 B)
   - `flash_run_1_4K0907557G_0003.candump.stale.142951` (143 B)
   - `flash_run_1_4K0907557G_0003.candump.stale.151221` (36 B)
   - `flash_run_1_4K0907557G_0003.log` (0 B)
   No byte-offset map exists. Two ways forward:
   - **Option A (self-contained):** parse `/Users/rabbit/sniffer/mm_MAPS_upload.log` directly in next session — extract the 64 CAL TransferData payloads, strip the `0x36 <BC>` header, concat to `/tmp/cal_ciphertext.bin`. Adds ~5 min to budget.
   - **Option B:** ask user where they want the byte-offset map / pre-extracted ciphertext to live, then build it once and write `SUMMARY.md`.
3. **Oracle path correction:** the experiment prompt says oracle is under `hw_reference/.../<RS7_flashed.bin>` — actually it's at `/Users/rabbit/sniffer/WUAPCBF28NN902533_4K0907557G__0003.bin`. Use that.

---

## Useful sanity-check fingerprints (from analysis doc §2.4)

When the experiment runs, sanity-check against these before trying decrypt:

- **CAL ciphertext expected size:** ~262 KB on the wire. Exact byte
  count depends on parsed CF reassembly — count blocks 0x01..0x40
  (64 blocks), each FirstFrame says ISO-TP-length, sum minus the
  `36 <BC>` 2 bytes per block.
- **CAL plaintext (oracle) size:** 1,572,864 B (0x180000) exactly.
- **CAL plaintext CRC32 (verified by analysis doc):** `45 85 0B EA` →
  `zlib.crc32(oracle) == 0x45850BEA`. Use this to confirm oracle slice
  is correct BEFORE running decrypt — if CRC32 doesn't match, the
  slice bounds are wrong.
- **CAL first 16 plaintext bytes:** unknown — read them out of the
  binary in the next session and record here for IV-experiment
  debugging (the first 16 B of decrypted output should match these
  if IV is correct, assuming CBC starts at offset 0 of the
  reassembled stream).

---

## IV hypotheses (from experiment spec)

1. Zero IV (16 bytes 0x00).
2. Block-counter-derived: 4-byte counter big-endian, left-padded with
   zeros to 16 B. Question: counter per block (resets to 0x01 per
   section, so first block IV = `0x00..00 00 00 00 01`) or per section
   (`0x00..00 00 00 00 06` for CAL, since BID = 0x06)?
3. Fixed Bosch: `00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F`.
4. Hermes-suggested (TBD).

Other candidates worth trying if first three fail (don't burn budget
on these without Hermes's input first):
- IV = first 16 B of session seed (`C361B058` for FULL flash,
  `168BC5E2` for MAPS). Unlikely but cheap.
- IV = AES key itself. Unlikely.
- IV derived from VIN / boxcode. Plausible — MM tool knows both.
- Per-block CTR mode with nonce = block counter.

---

## Hard rules to honour (from project CLAUDE.md)

- AES keys live in `secrets/` (gitignored). **Do not write the key
  bytes into any artifact** — the experiment can load and use them
  in-memory but the report files must only fingerprint them (first 4
  bytes hex, or a SHA-256 short hash). Same for the IV if it turns out
  to be derived from a key.
- No magic numbers in firmware. The experiment outputs go to `/tmp/`
  and `/Users/rabbit/esp/obd/` (status logs) — not firmware source.
- Mandatory progress logging: when experiment ends, append to
  `~/esp/obd/file-update-2026-05-10.md` and `~/esp/obd/status-2026-05-10.md`.

---

## Open questions for the user (resume by asking)

1. **Hermes startup.** Is the agent supposed to be running as a server
   already, and the lsof empty result means we should start it? Or is
   `hermes` a CLI that fronts a remote server (in which case where is
   the endpoint URL configured)?
2. **Endpoint URL + model name + token.** Need exact values.
3. **Capture-fixture strategy.** Should I parse
   `/Users/rabbit/sniffer/mm_MAPS_upload.log` directly (Option A above)
   or use a pre-existing tool / location (Option B)?

---

## Original prompt to re-issue

The full experiment spec the user originally gave is too long to
reproduce verbatim here — see the prior session transcript. Key
constraints to keep:
- 30 min wall-clock total budget, 25 min hard cap on Phase 2.
- ≤10 s timeout per Hermes call; on failure continue without it.
- Don't commit. Don't modify `firmware/src/`.
- Output artifacts in `/tmp/`: `cal_ciphertext.bin`, `cal_oracle.bin`,
  `cal_decrypt_<HYP>.bin`, `cal_trial_<HYP>.md`, `hermes_preflight.md`,
  `hermes_postmortem.md`, `dataformat_2a_experiment.md`.
- Append summary paragraph to `~/esp/obd/file-update-2026-05-10.md`.

---

*Created 2026-05-10 by the prior Claude session before
permission-prompt context switch. Resume from "Open questions"
section.*
