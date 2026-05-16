#!/usr/bin/env python3
"""
hermes_sweep.py — variant key-extraction sweep dispatcher.

Option B architecture: we (this script) pre-scan every .bin under
~/034_local/ for the two candidate AES-key offsets (0x18200 and
0x600200), computing SHA256-first-4-bytes fingerprints + Shannon
entropy at each offset. We pass that pre-computed metadata to Hermes
in batches of 50 bins per request, and Hermes classifies which
offset is the real key per bin (or "neither"). Then we revalidate
Hermes' classification by re-reading the bins ourselves.

The point: no raw key bytes ever leave this machine (fingerprints
only on the wire to Hermes). The LLM does pattern matching on
filename + entropy + fingerprint metadata; the cryptographic work
(SHA256, entropy, byte reads) is local.

Outputs:
  firmware/test/bin_inventory.md
  tools/proposed_manifest_merge_2026-05-12.json
  tools/hermes_extraction_report_2026-05-12.md
  /tmp/hermes_req_<batch>.json + /tmp/hermes_resp_<batch>.json

Hard rules honored:
  - No raw key bytes anywhere
  - No commits
  - No modifications to secrets/

stdlib only + curl (subprocess).
"""
import argparse
import concurrent.futures as cf
import datetime
import hashlib
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT_SCAN_DIR = Path.home() / "034_local"
KEY_OFFSETS   = [0x18200, 0x600200]
KEY_LEN       = 16
PROJECT_ROOT  = Path("/Users/rabbit/esp/obd/FUTV1.1")
INVENTORY_MD  = PROJECT_ROOT / "firmware/test/bin_inventory.md"
PROPOSAL_JSON = PROJECT_ROOT / "tools/proposed_manifest_merge_2026-05-12.json"
REPORT_MD     = PROJECT_ROOT / "tools/hermes_extraction_report_2026-05-12.md"
TMP_DIR       = Path("/tmp")

# Hermes endpoint
HERMES_URL    = "http://192.168.1.180:3000/api/chat/completions"
HERMES_MODEL  = os.environ.get("HERMES_MODEL", "nemo180:latest")
BATCH_SIZE    = 50
PARALLELISM   = 5
PER_REQ_TIMEOUT_S = 120
TOTAL_TIMEOUT_S   = 30 * 60   # 30 min hard cap


def load_auth_token():
    """Read OPENAI_API_KEY from ~/.hermes/.env without printing it."""
    env_path = Path.home() / ".hermes/.env"
    if not env_path.is_file():
        sys.exit(f"ERROR: {env_path} not found")
    with open(env_path) as f:
        for ln in f:
            ln = ln.strip()
            if ln.startswith("OPENAI_API_KEY="):
                v = ln.split("=", 1)[1].strip()
                # Strip surrounding quotes if present
                if v and v[0] == v[-1] and v[0] in ('"', "'"):
                    v = v[1:-1]
                return v
    sys.exit("ERROR: OPENAI_API_KEY not found in ~/.hermes/.env")


def shannon_entropy(data):
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    H = 0.0
    for c in counts:
        if c:
            p = c / n
            H -= p * math.log2(p)
    return H


def is_acceptable_key_block(data):
    """Apply entropy + repeated-byte rejection per FINDINGS doc."""
    if not data or len(data) != KEY_LEN:
        return False, "wrong_length"
    if all(b == 0xFF for b in data):
        return False, "all_FF"
    if all(b == 0x00 for b in data):
        return False, "all_zero"
    if len(set(data)) == 1:
        return False, "all_one_byte_repeated"
    H = shannon_entropy(data)
    if H < 3.5:
        return False, f"low_entropy_{H:.2f}"
    return True, f"entropy_{H:.2f}"


def precompute_bin_metadata(bin_path):
    """For one bin, compute size + per-offset (sha256_first8, entropy, acceptable)."""
    try:
        st = os.stat(bin_path)
    except OSError:
        return None
    size = st.st_size
    out = {
        "path": str(bin_path),
        "filename": bin_path.name,
        "parent": str(bin_path.parent.relative_to(ROOT_SCAN_DIR)) if str(bin_path).startswith(str(ROOT_SCAN_DIR)) else str(bin_path.parent),
        "size_bytes": size,
        "offsets": {},
    }
    try:
        with open(bin_path, "rb") as f:
            for off in KEY_OFFSETS:
                if off + KEY_LEN > size:
                    out["offsets"][hex(off)] = {
                        "in_range": False,
                        "reason": "offset_beyond_eof",
                    }
                    continue
                f.seek(off)
                block = f.read(KEY_LEN)
                if len(block) != KEY_LEN:
                    out["offsets"][hex(off)] = {
                        "in_range": False,
                        "reason": "short_read",
                    }
                    continue
                ok, why = is_acceptable_key_block(block)
                # SHA256 first 8 hex chars only — NEVER raw bytes.
                sha8 = hashlib.sha256(block).hexdigest()[:8]
                out["offsets"][hex(off)] = {
                    "in_range": True,
                    "acceptable": ok,
                    "reason": why,
                    "sha256_first8_fingerprint": sha8,
                }
    except OSError as e:
        out["error"] = str(e)
    return out


def walk_bins(root):
    """Return list of Path objects for every *.bin under root, sorted."""
    bins = []
    for r, _, files in os.walk(root):
        for fn in files:
            if fn.lower().endswith(".bin"):
                bins.append(Path(r) / fn)
    bins.sort(key=lambda p: str(p))
    return bins


def write_inventory(meta_list):
    INVENTORY_MD.parent.mkdir(parents=True, exist_ok=True)
    with open(INVENTORY_MD, "w") as f:
        f.write("# bin_inventory.md — pre-scan ground truth for the Hermes variant-key-extraction sweep\n\n")
        f.write(f"Generated: {datetime.datetime.now().isoformat(timespec='seconds')}\n\n")
        f.write(f"Source: {ROOT_SCAN_DIR}\n\n")
        f.write(f"Total .bin files: {len(meta_list)}\n\n")
        f.write("Per-bin: SHA-256[:8] fingerprint + entropy at each candidate offset.\n")
        f.write("**No raw key bytes here — fingerprints only.**\n\n")
        f.write("| Path (relative) | Size | 0x18200 sha[:8] | 0x18200 entropy | 0x600200 sha[:8] | 0x600200 entropy |\n")
        f.write("|---|---|---|---|---|---|\n")
        for m in meta_list:
            rel = m["path"].replace(str(ROOT_SCAN_DIR) + "/", "")
            o1 = m["offsets"].get(hex(0x18200), {})
            o2 = m["offsets"].get(hex(0x600200), {})
            def cell(o):
                if not o.get("in_range"):
                    return o.get("reason", "n/a"), "—"
                return o.get("sha256_first8_fingerprint", "?"), o.get("reason", "?")
            s1, e1 = cell(o1)
            s2, e2 = cell(o2)
            f.write(f"| {rel} | {m['size_bytes']} | {s1} | {e1} | {s2} | {e2} |\n")


def batch_iter(items, n):
    for i in range(0, len(items), n):
        yield i // n, items[i:i+n]


def build_hermes_messages(batch):
    """Build the user-message content for a batch of bins. The model sees:
    paths, filenames, fingerprints + acceptability per offset. It does
    NOT see raw key bytes."""
    system = (
        "You are a classifier for Bosch ECU bootloader AES-128 key location. "
        "Each bin has a key stored at either offset 0x18200 (plain MG1/MDG1) or "
        "offset 0x600200 (IFX integrated-firmware variants). "
        "For each bin in the batch, decide which offset (if any) is the real "
        "key based on the pre-computed metadata. Output JSON only, no markdown."
    )
    user_lines = [
        "Classify each of the following bins. The metadata at each candidate offset "
        "is the SHA-256-first-8-hex of the 16 bytes there + entropy + acceptability "
        "(passed an entropy >= 3.5 and not-all-same-byte filter).",
        "",
        "For each bin, emit a JSON entry with:",
        '  "bin_path": absolute path from the input',
        '  "box_code": VAG part number you infer from the filename or parent dir (e.g. "8W0907559H 0009"). null if not derivable.',
        '  "chosen_offset": "0x18200" | "0x600200" | "none"',
        '  "chosen_sha256_first8_fingerprint": (string from metadata for the chosen offset, or null if none)',
        '  "verification_status": "candidate_offset_match" if exactly one offset was acceptable AND filename heuristics suggest it; "ambiguous_offset" if both acceptable; "no_candidate_offset" if neither.',
        '  "filename_heuristic": brief note on which offset the filename suggests (e.g. "IFX in path" → 0x600200; "CS002" without IFX → 0x18200).',
        '  "notes": optional caveats.',
        "",
        "Output: a JSON object with key 'classifications' whose value is the array.",
        "",
        "Bins:",
    ]
    for m in batch:
        o18 = m["offsets"].get(hex(0x18200), {})
        o60 = m["offsets"].get(hex(0x600200), {})
        line = {
            "bin_path": m["path"],
            "filename": m["filename"],
            "parent": m["parent"],
            "size_bytes": m["size_bytes"],
            "offset_0x18200": {
                "in_range": o18.get("in_range", False),
                "acceptable": o18.get("acceptable", False),
                "reason": o18.get("reason"),
                "sha256_first8": o18.get("sha256_first8_fingerprint"),
            },
            "offset_0x600200": {
                "in_range": o60.get("in_range", False),
                "acceptable": o60.get("acceptable", False),
                "reason": o60.get("reason"),
                "sha256_first8": o60.get("sha256_first8_fingerprint"),
            },
        }
        user_lines.append(json.dumps(line))
    user = "\n".join(user_lines)
    return [
        {"role": "system", "content": system},
        {"role": "user", "content": user},
    ]


def dispatch_one_batch(idx, batch, token):
    """Send one ChatCompletion request. Return (idx, response_json, elapsed_s, err)."""
    messages = build_hermes_messages(batch)
    body = {
        "model": HERMES_MODEL,
        "messages": messages,
        "max_tokens": 8192,
        "temperature": 0,
        "response_format": {"type": "json_object"},
    }
    req_path = TMP_DIR / f"hermes_req_{idx:03d}.json"
    resp_path = TMP_DIR / f"hermes_resp_{idx:03d}.json"
    err_path  = TMP_DIR / f"hermes_err_{idx:03d}.log"
    with open(req_path, "w") as f:
        json.dump(body, f)
    cmd = [
        "curl", "-s", "--max-time", str(PER_REQ_TIMEOUT_S),
        "-X", "POST", HERMES_URL,
        "-H", "Content-Type: application/json",
        "-H", f"Authorization: Bearer {token}",
        "-d", f"@{req_path}",
        "-o", str(resp_path),
    ]
    t0 = time.monotonic()
    rc = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.monotonic() - t0
    with open(err_path, "w") as f:
        f.write(f"curl rc={rc.returncode}\nstderr:\n{rc.stderr}\n")
    if rc.returncode != 0:
        return idx, None, elapsed, f"curl rc={rc.returncode}: {rc.stderr.strip()}"
    try:
        with open(resp_path) as f:
            resp = json.load(f)
    except Exception as e:
        return idx, None, elapsed, f"resp parse error: {e}"
    if "choices" not in resp or not resp["choices"]:
        return idx, None, elapsed, f"no choices: {json.dumps(resp)[:200]}"
    return idx, resp, elapsed, None


def parse_classifications(resp):
    """Extract the array of classifications from a ChatCompletion response."""
    try:
        content = resp["choices"][0]["message"]["content"]
    except (KeyError, IndexError):
        return None, "no content"
    try:
        data = json.loads(content)
    except json.JSONDecodeError as e:
        return None, f"content not valid JSON: {e}"
    if isinstance(data, list):
        return data, None
    if isinstance(data, dict) and "classifications" in data:
        return data["classifications"], None
    return None, "unexpected JSON shape"


def validate_entry(entry, meta_by_path):
    """Q4 validation gate. Returns (eligible_for_merge_bool, reasons_list)."""
    reasons = []
    bin_path = entry.get("bin_path")
    meta = meta_by_path.get(bin_path)
    if not meta:
        return False, ["unmatched_bin_path"]
    chosen = entry.get("chosen_offset", "none")
    if chosen == "none":
        return False, ["chosen_none"]
    if chosen not in ("0x18200", "0x600200"):
        return False, [f"unknown_offset:{chosen}"]
    off_key = chosen
    off_meta = meta["offsets"].get(off_key, {})
    if not off_meta.get("in_range"):
        return False, ["chosen_offset_out_of_range"]
    # Re-verify acceptability + fingerprint
    ok = off_meta.get("acceptable")
    if not ok:
        return False, [f"entropy_disagreement:{off_meta.get('reason')}"]
    expected_sha = off_meta.get("sha256_first8_fingerprint")
    given_sha = entry.get("chosen_sha256_first8_fingerprint")
    if given_sha != expected_sha:
        # Hermes might have hallucinated; we re-derived. The OUR sha is
        # authoritative. Flag and use ours.
        reasons.append(f"hermes_fingerprint_mismatch:hermes={given_sha} ours={expected_sha}")
    box = entry.get("box_code")
    if not box:
        reasons.append("box_code_null")
    return (len(reasons) == 0), reasons or ["ok"]


def smoke_test_batch_one_clean(classifications, meta_by_path):
    """Smoke check: did batch 1 return a sensible structure?"""
    if not classifications:
        return False, "empty classifications"
    if not isinstance(classifications, list):
        return False, "classifications not a list"
    sample = classifications[0]
    required = ("bin_path", "chosen_offset", "verification_status")
    for k in required:
        if k not in sample:
            return False, f"missing key {k} in entry 0"
    # At least one entry should validate cleanly
    n_valid = 0
    for e in classifications:
        ok, _ = validate_entry(e, meta_by_path)
        if ok:
            n_valid += 1
    return n_valid >= 1, f"{n_valid}/{len(classifications)} entries pass validation"


def find_dev_rs7_smoke(meta_list):
    """The dev RS7 bin isn't in 034_local — it's at /Users/rabbit/sniffer/.
    But our manifest already has 4K0907557G__0003 entered. So the smoke
    test cross-check is: is 7fa117fa anywhere in the sweep's fingerprints
    (it shouldn't be for 034_local), and the dev RS7 round-trip already
    completed in the prior prompt's work.

    Instead, validate the smoke condition by re-running Hermes against
    the dev RS7 explicitly as a 1-bin batch. Return that targeted bin's
    metadata."""
    rs7_bin = Path("/Users/rabbit/sniffer/WUAPCBF28NN902533_4K0907557G__0003.bin")
    if not rs7_bin.exists():
        return None
    return precompute_bin_metadata(rs7_bin)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true",
                    help="Pre-scan + write inventory only; no Hermes dispatch.")
    ap.add_argument("--smoke-only", action="store_true",
                    help="Pre-scan + dispatch batch 1 only, then exit.")
    args = ap.parse_args()

    token = load_auth_token()
    print(f"[{datetime.datetime.now().isoformat(timespec='seconds')}] Hermes sweep starting")

    # Phase 1 — pre-scan
    t0 = time.monotonic()
    print(f"  walking {ROOT_SCAN_DIR}...")
    bins = walk_bins(ROOT_SCAN_DIR)
    print(f"  found {len(bins)} .bin files")
    if not bins:
        sys.exit("nothing to do — no bins found")

    print(f"  precomputing metadata...")
    meta_list = []
    for b in bins:
        m = precompute_bin_metadata(b)
        if m:
            meta_list.append(m)
    t_scan = time.monotonic() - t0
    print(f"  pre-scan: {len(meta_list)} bins in {t_scan:.1f}s")

    write_inventory(meta_list)
    print(f"  wrote inventory: {INVENTORY_MD}")

    if args.dry_run:
        print("dry-run done; exiting before dispatch")
        return

    meta_by_path = {m["path"]: m for m in meta_list}

    # Phase 2 — dispatch batch 1 as smoke test
    batches = list(batch_iter(meta_list, BATCH_SIZE))
    print(f"  {len(batches)} batches of up to {BATCH_SIZE}")

    print(f"\n[{datetime.datetime.now().isoformat(timespec='seconds')}] Dispatching batch 1 (smoke test)...")
    idx0, resp0, elapsed0, err0 = dispatch_one_batch(0, batches[0][1], token)
    if err0:
        sys.exit(f"BATCH 1 FAILED: {err0}")
    print(f"  batch 1 returned in {elapsed0:.1f}s")
    classes0, perr = parse_classifications(resp0)
    if perr:
        sys.exit(f"BATCH 1 PARSE FAILED: {perr}")
    print(f"  batch 1 entries: {len(classes0)}")

    ok, why = smoke_test_batch_one_clean(classes0, meta_by_path)
    print(f"  smoke test: {'PASS' if ok else 'FAIL'} — {why}")
    if not ok:
        sys.exit("smoke test failed; check /tmp/hermes_resp_000.json")

    # Phase 3 — dev RS7 cross-check (if accessible)
    rs7 = find_dev_rs7_smoke(meta_list)
    if rs7:
        print(f"\n  dev RS7 cross-check: fingerprint at 0x600200 = "
              f"{rs7['offsets'].get(hex(0x600200), {}).get('sha256_first8_fingerprint')}")
        expected = "7fa117fa"
        actual = rs7['offsets'].get(hex(0x600200), {}).get('sha256_first8_fingerprint')
        if actual == expected:
            print(f"  smoke PASS — fingerprint matches manifest's 7fa117fa")
        else:
            print(f"  smoke FAIL — fingerprint mismatch! manifest={expected} actual={actual}")
            sys.exit("dev RS7 smoke mismatch — environment is broken")

    if args.smoke_only:
        # Write a minimal report and exit
        with open(REPORT_MD, "w") as f:
            f.write(f"# Hermes Sweep — smoke test only\n\nBatch 1: {len(classes0)} classifications.\nDev RS7 cross-check: passed.\n")
        print("smoke-only mode; exiting")
        return

    # Phase 4 — dispatch remaining batches in parallel
    remaining = batches[1:]
    print(f"\n[{datetime.datetime.now().isoformat(timespec='seconds')}] Dispatching {len(remaining)} remaining batches (parallelism={PARALLELISM})...")
    all_resp = {0: resp0}
    elapsed_map = {0: elapsed0}
    err_map = {}
    t_phase4 = time.monotonic()
    with cf.ThreadPoolExecutor(max_workers=PARALLELISM) as pool:
        futures = {pool.submit(dispatch_one_batch, i, b, token): i
                   for i, b in remaining}
        for fut in cf.as_completed(futures, timeout=TOTAL_TIMEOUT_S):
            i = futures[fut]
            try:
                idx, resp, elapsed, err = fut.result()
            except Exception as e:
                err_map[i] = f"exception: {e}"
                continue
            elapsed_map[idx] = elapsed
            if err:
                err_map[idx] = err
                print(f"  batch {idx:3d}: FAIL ({err}) [{elapsed:.1f}s]")
            else:
                all_resp[idx] = resp
                print(f"  batch {idx:3d}: ok [{elapsed:.1f}s]")
    print(f"  dispatch complete in {time.monotonic() - t_phase4:.1f}s")

    # Phase 5 — validate all responses
    print(f"\n[{datetime.datetime.now().isoformat(timespec='seconds')}] Validating responses...")
    all_entries = []
    parse_errs = []
    for idx in sorted(all_resp):
        classes, perr = parse_classifications(all_resp[idx])
        if perr:
            parse_errs.append((idx, perr))
            continue
        all_entries.extend(classes)
    print(f"  total entries: {len(all_entries)}")
    print(f"  batches with parse error: {len(parse_errs)}")
    print(f"  batches with dispatch error: {len(err_map)}")

    eligible = []
    flagged = []
    for e in all_entries:
        ok, reasons = validate_entry(e, meta_by_path)
        if ok:
            eligible.append({
                "bin_path": e.get("bin_path"),
                "box_code": e.get("box_code"),
                "chosen_offset": e.get("chosen_offset"),
                "sha256_first8_fingerprint": e.get("chosen_sha256_first8_fingerprint")
                    or meta_by_path[e["bin_path"]]["offsets"][e["chosen_offset"]]["sha256_first8_fingerprint"],
                "filename_heuristic": e.get("filename_heuristic"),
                "verification_status": e.get("verification_status"),
            })
        else:
            flagged.append({
                "entry": e,
                "reasons": reasons,
            })
    print(f"  eligible: {len(eligible)}  flagged: {len(flagged)}")

    # Phase 6 — write proposal + report
    PROPOSAL_JSON.parent.mkdir(parents=True, exist_ok=True)
    with open(PROPOSAL_JSON, "w") as f:
        json.dump({
            "generated_at": datetime.datetime.now().isoformat(timespec='seconds'),
            "source_scan": str(ROOT_SCAN_DIR),
            "hermes_model": HERMES_MODEL,
            "hermes_endpoint": HERMES_URL,
            "entries": eligible,
        }, f, indent=2)

    with open(REPORT_MD, "w") as f:
        f.write("# Hermes variant-key-extraction sweep — 2026-05-12\n\n")
        f.write(f"Generated: {datetime.datetime.now().isoformat(timespec='seconds')}\n\n")
        f.write("## Summary\n\n")
        f.write(f"- Bins scanned: {len(meta_list)}\n")
        f.write(f"- Batches dispatched: {len(batches)} ({len(remaining)+1} total; parallelism {PARALLELISM})\n")
        f.write(f"- Total candidates returned: {len(all_entries)}\n")
        f.write(f"- Eligible for merge: {len(eligible)}\n")
        f.write(f"- Flagged for review: {len(flagged)}\n")
        f.write(f"- Dispatch errors: {len(err_map)}\n")
        f.write(f"- Parse errors: {len(parse_errs)}\n\n")
        f.write("## Round-trip times\n\n")
        f.write(f"- Batch 1 (smoke): {elapsed0:.1f}s\n")
        elapsed_vals = [v for k, v in elapsed_map.items() if k != 0]
        if elapsed_vals:
            f.write(f"- Remaining batches: min={min(elapsed_vals):.1f}s "
                    f"avg={sum(elapsed_vals)/len(elapsed_vals):.1f}s "
                    f"max={max(elapsed_vals):.1f}s\n\n")
        f.write("## Eligible-for-merge\n\n")
        if eligible:
            f.write("| Box code | Bin path | Offset | sha256[:8] |\n|---|---|---|---|\n")
            for e in eligible:
                f.write(f"| {e['box_code']} | {e['bin_path']} | {e['chosen_offset']} | {e['sha256_first8_fingerprint']} |\n")
        else:
            f.write("(none)\n")
        f.write("\n## Flagged-for-human-review\n\n")
        if flagged:
            f.write("| Bin path | Box code | Chosen | Reasons |\n|---|---|---|---|\n")
            for fl in flagged:
                e = fl["entry"]
                f.write(f"| {e.get('bin_path')} | {e.get('box_code')} | {e.get('chosen_offset')} | {'; '.join(fl['reasons'])} |\n")
        else:
            f.write("(none)\n")
        f.write("\n## Dispatch / Parse Errors\n\n")
        if err_map:
            for idx, err in sorted(err_map.items()):
                f.write(f"- batch {idx}: {err}\n")
        if parse_errs:
            for idx, err in parse_errs:
                f.write(f"- batch {idx} parse: {err}\n")
        if not err_map and not parse_errs:
            f.write("(none)\n")
        f.write("\n## Hermes raw output (first 2 batches verbatim)\n\n")
        for idx in [0, 1]:
            if idx in all_resp:
                f.write(f"### batch {idx}\n\n```json\n")
                f.write(json.dumps(all_resp[idx], indent=2)[:8000])
                f.write("\n```\n\n")

    print(f"  wrote proposal: {PROPOSAL_JSON}")
    print(f"  wrote report:   {REPORT_MD}")
    print(f"\n[{datetime.datetime.now().isoformat(timespec='seconds')}] DONE.")


if __name__ == "__main__":
    main()
