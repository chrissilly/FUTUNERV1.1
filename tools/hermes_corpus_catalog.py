#!/usr/bin/env python3
"""
hermes_corpus_catalog.py

Hard, long-running corpus characterization sweep across the
vag_mdg1_drive_pull/ archive (~3,521 bins). Runs unattended for
many hours; produces a structured catalog of every bin's profile.

Three passes, each independently controllable via --pass:

  Pass 1: Per-bin deep profile
    For each bin, extract metadata from multiple Bosch regions,
    compute multi-region fingerprints, send rich context to
    Hermes for interpretation. Output: ECU family, vehicle
    inference, software/cal versions, tool signature, anomalies.

  Pass 2: Cross-bin family clustering
    Group bins by inferred family from Pass 1. Within each family,
    Hermes identifies canonical-vs-derivative dumps, twin bins
    (same SW different cal), and version progression.

  Pass 3: Reject deep-dive
    For each of the 462 bins that had no key at the documented
    offsets, Hermes scans for high-entropy 16-byte windows at
    non-documented offsets and proposes candidate alternative
    key offsets with justification.

All passes are resumable. Designed to run in tmux/screen:
  tmux new -s catalog
  cd ~/esp/obd/FUTV1.1
  nohup python3 tools/hermes_corpus_catalog.py --pass all > /dev/null 2>&1 &
  # detach with Ctrl-b d

Outputs:
  tools/hermes_corpus_catalog_<ts>.json         (Pass 1 per-bin profiles)
  tools/hermes_corpus_clusters_<ts>.json        (Pass 2 family groupings)
  tools/hermes_corpus_rejects_<ts>.json         (Pass 3 offset proposals)
  tools/hermes_corpus_summary_<ts>.md           (human-readable rollup)
  tools/hermes_corpus_catalog.progress          (resumable, per-pass)
  tools/hermes_corpus_catalog.log               (per-run log)

Usage:
  python3 tools/hermes_corpus_catalog.py --pass 1
  python3 tools/hermes_corpus_catalog.py --pass 2
  python3 tools/hermes_corpus_catalog.py --pass 3
  python3 tools/hermes_corpus_catalog.py --pass all
  python3 tools/hermes_corpus_catalog.py --dry-run --pass 1 --limit 5
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
from collections import defaultdict
from pathlib import Path

try:
    import requests
except ImportError:
    print("Install: pip3 install requests", file=sys.stderr)
    sys.exit(1)


# ─── CONFIG ────────────────────────────────────────────────────────────────
# All constants live here. Items marked "approval" need Sean's sign-off
# before being baked in permanently.

CONFIG = {
    "hermes_base_url": os.environ.get(
        "HERMES_BASE_URL", "http://192.168.1.180:3000/api"
    ),
    "hermes_model": os.environ.get("HERMES_MODEL", "nemo180:latest"),
    "hermes_env_file": os.path.expanduser("~/.hermes/.env"),
    "request_timeout_sec": 180,
    "max_retries_per_batch": 3,
    "retry_backoff_sec": 5,

    # Batching — different per pass since context size differs.
    # Approval needed before lock-in.
    "pass1_batch_size": 10,   # rich per-bin context, smaller batches
    "pass2_batch_size": 1,    # one family at a time
    "pass3_batch_size": 5,    # entropy analysis is dense

    # Pass 1: regions to extract metadata + strings from
    # (offset, length, label). Labels are passed to Hermes for context.
    "pass1_metadata_regions": [
        (0x000000, 0x10000, "header_64K"),
        (0x017000, 0x02000, "near_plain_key_offset"),
        (0x080000, 0x02000, "softwareversion_zone"),
        (0x100000, 0x04000, "ASW1_header"),
        (0x300000, 0x04000, "ASW2_header"),
        (0x500000, 0x04000, "CBOOT_header"),
        (0x600000, 0x02000, "near_IFX_key_offset"),
        (0x700000, 0x02000, "CAL_footer_zone"),
    ],

    # Pass 3: candidate offsets to scan for high-entropy 16-byte windows
    # (in addition to the two documented offsets). Stride of 16 bytes.
    "pass3_candidate_offsets": [
        (0x010000, 0x020000, 16),  # range start, end, stride
        (0x100000, 0x110000, 16),
        (0x500000, 0x510000, 16),
        (0x600000, 0x610000, 16),
        (0x780000, 0x800000, 16),  # tail of typical 8MB image
    ],
    "pass3_entropy_threshold": 3.5,
    "pass3_max_proposals_per_bin": 5,

    "min_ascii_run_length": 6,
    "max_strings_per_region": 15,

    "vag_partno_pattern": re.compile(
        r"\b[0-9][A-Z0-9][0-9][A-Z0-9]\d{6}[A-Z]?(?:\s+\d{3,4})?\b"
    ),
    "bosch_swversion_pattern": re.compile(
        r"\b\d{4}[A-Z0-9]{2,8}\b"
    ),

    # Input — the existing inventory (per-bin pre-scan ground truth)
    "default_inventory_input": "firmware/test/bin_inventory.md",
    "default_proposal_input": "tools/proposed_manifest_merge_2026-05-12.json",
    # Real archive lives at /Users/rabbit/034_local (164 GB of family
    # folders + .zips). hw_reference/vag_mdg1_drive_pull/ is just a
    # symlinked curated view. Env-override via FUTUNER_ARCHIVE_ROOT.
    "archive_root": os.environ.get(
        "FUTUNER_ARCHIVE_ROOT", "/Users/rabbit/034_local"
    ),

    "output_pass1_template": "tools/hermes_corpus_catalog_{ts}.json",
    "output_pass2_template": "tools/hermes_corpus_clusters_{ts}.json",
    "output_pass3_template": "tools/hermes_corpus_rejects_{ts}.json",
    "output_summary_template": "tools/hermes_corpus_summary_{ts}.md",
    "progress_file": "tools/hermes_corpus_catalog.progress",
    "log_file": "tools/hermes_corpus_catalog.log",
}


# ─── COMMON HELPERS ────────────────────────────────────────────────────────

def load_api_key() -> str:
    key = os.environ.get("HERMES_API_KEY") or os.environ.get("OPENAI_API_KEY")
    if key:
        return key
    env_path = Path(CONFIG["hermes_env_file"])
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            line = line.strip()
            if line.startswith(("OPENAI_API_KEY=", "HERMES_API_KEY=")):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise RuntimeError(
        "No API key found. Set HERMES_API_KEY or OPENAI_API_KEY in env "
        "or in ~/.hermes/.env"
    )


def log(msg: str, log_path: Path) -> None:
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(log_path, "a") as f:
        f.write(line + "\n")


def load_progress(progress_path: Path, pass_name: str) -> set:
    if not progress_path.exists():
        return set()
    done = set()
    for line in progress_path.read_text().splitlines():
        if line.startswith(f"{pass_name}:"):
            done.add(line.split(":", 1)[1])
    return done


def append_progress(progress_path: Path, pass_name: str, items: list) -> None:
    with open(progress_path, "a") as f:
        for it in items:
            f.write(f"{pass_name}:{it}\n")


def hermes_call(prompt: str, api_key: str) -> str:
    url = CONFIG["hermes_base_url"].rstrip("/") + "/chat/completions"
    payload = {
        "model": CONFIG["hermes_model"],
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0,
        "max_tokens": 8192,
    }
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    last_err = None
    for attempt in range(CONFIG["max_retries_per_batch"]):
        try:
            r = requests.post(
                url, headers=headers, json=payload,
                timeout=CONFIG["request_timeout_sec"],
            )
            r.raise_for_status()
            return r.json()["choices"][0]["message"]["content"]
        except Exception as e:
            last_err = e
            time.sleep(CONFIG["retry_backoff_sec"] * (attempt + 1))
    raise RuntimeError(f"Hermes call failed: {last_err}")


def parse_hermes_array(text: str) -> list:
    try:
        parsed = json.loads(text)
        if isinstance(parsed, list):
            return parsed
    except json.JSONDecodeError:
        pass
    s, e = text.find("["), text.rfind("]")
    if s >= 0 and e > s:
        return json.loads(text[s : e + 1])
    raise ValueError("No parseable JSON array in Hermes response")


# ─── BIN ANALYSIS HELPERS ──────────────────────────────────────────────────

def shannon_entropy(buf: bytes) -> float:
    if not buf:
        return 0.0
    counts = [0] * 256
    for b in buf:
        counts[b] += 1
    n = len(buf)
    import math
    h = 0.0
    for c in counts:
        if c:
            p = c / n
            h -= p * math.log2(p)
    return h


def extract_strings_regions(path: Path) -> dict:
    """Extract printable ASCII string runs from configured regions."""
    pattern = re.compile(
        rb"[\x20-\x7E]{%d,}" % CONFIG["min_ascii_run_length"]
    )
    result = {}
    try:
        size = path.stat().st_size
        with open(path, "rb") as f:
            for offset, length, label in CONFIG["pass1_metadata_regions"]:
                if offset >= size:
                    continue
                f.seek(offset)
                chunk = f.read(min(length, size - offset))
                runs = pattern.findall(chunk)
                ranked = sorted(
                    set(r.decode("ascii", errors="ignore") for r in runs),
                    key=lambda s: (
                        not bool(CONFIG["vag_partno_pattern"].search(s)),
                        not bool(CONFIG["bosch_swversion_pattern"].search(s)),
                        -len(s),
                    ),
                )
                result[label] = ranked[: CONFIG["max_strings_per_region"]]
    except Exception as e:
        result["_error"] = str(e)
    return result


def scan_candidate_key_offsets(path: Path) -> list:
    """Pass 3 helper: scan non-documented offsets for high-entropy windows."""
    proposals = []
    try:
        size = path.stat().st_size
        with open(path, "rb") as f:
            for start, end, stride in CONFIG["pass3_candidate_offsets"]:
                end = min(end, size - 16)
                for off in range(start, end, stride):
                    f.seek(off)
                    win = f.read(16)
                    if len(win) < 16:
                        continue
                    if win == bytes(16) or win == b"\xff" * 16:
                        continue
                    if len(set(win)) == 1:
                        continue
                    h = shannon_entropy(win)
                    if h >= CONFIG["pass3_entropy_threshold"]:
                        proposals.append({
                            "offset": off,
                            "entropy": round(h, 3),
                            "first8_sha": hashlib.sha256(win).hexdigest()[:8],
                        })
    except Exception:
        return []
    proposals.sort(key=lambda p: -p["entropy"])
    return proposals[: CONFIG["pass3_max_proposals_per_bin"]]


# ─── PASS 1: PER-BIN DEEP PROFILE ──────────────────────────────────────────

def build_pass1_prompt(batch: list) -> str:
    return (
        "You are characterizing VAG ECU bin dumps. For each entry below, "
        "produce a structured profile based on filename, parent_dir, "
        "file_size, and embedded_strings (organized by Bosch metadata "
        "region).\n\n"
        "Bosch ECU families include: MED9, MED17, MEDC17, EDC17, MG1 "
        "(generic), MG1CS001/2/8/11, MG1CP002, MDG1, MD1CP004, MD1CP014, "
        "and TCU families DQ250/DQ381/DQ500/DQ381 G2. VAG box codes "
        "follow patterns like '4K0907557G' (10-char part number) "
        "optionally followed by a 3-4 digit hardware version.\n\n"
        "For each entry, return the JSON schema below. Use null where you "
        "can't determine. Be concise in reasoning.\n\n"
        "Schema per entry:\n"
        "  {\n"
        "    \"bin_path\": \"<copy from input>\",\n"
        "    \"box_code\": \"<inferred VAG part number>\" or null,\n"
        "    \"ecu_family\": \"<MED17 | EDC17 | MG1 | MDG1 | TCU | ...>\","
        " or null,\n"
        "    \"ecu_variant\": \"<MG1CS002 | MD1CP014 | DQ381 | ...>\" "
        "or null,\n"
        "    \"vehicle_inference\": \"<best-guess vehicle/model/year>\" "
        "or null,\n"
        "    \"sw_version\": \"<software version string>\" or null,\n"
        "    \"cal_version\": \"<calibration version string>\" or null,\n"
        "    \"tool_signature\": \"<MagicMotorsport | EVC | Autotuner | "
        "Bosch | unknown>\",\n"
        "    \"dump_type\": \"<full_8mb | partial | cal_only | "
        "research | unknown>\",\n"
        "    \"anomalies\": [\"<short descriptors>\"],\n"
        "    \"confidence\": \"high | medium | low | uncertain\",\n"
        "    \"reasoning\": \"<one or two sentences>\"\n"
        "  }\n\n"
        "Output: pure JSON array, same order as input, no markdown.\n\n"
        "Input:\n" + json.dumps(batch, indent=2)
    )


def run_pass1(args, project_root: Path, log_path: Path,
              progress_path: Path, ts: str):
    output_path = project_root / CONFIG["output_pass1_template"].format(ts=ts)
    archive = project_root / CONFIG["archive_root"]
    if not archive.exists():
        log(f"Archive not found: {archive}", log_path)
        return

    bins = sorted(archive.rglob("*.bin"))
    log(f"Pass 1: found {len(bins)} bins in archive", log_path)

    done = load_progress(progress_path, "pass1") if args.resume else set()
    if done:
        bins = [b for b in bins if str(b) not in done]
        log(f"Pass 1: resume — {len(bins)} remaining", log_path)
    if args.limit:
        bins = bins[: args.limit]

    api_key = None if args.dry_run else load_api_key()

    results = []
    batch_size = CONFIG["pass1_batch_size"]
    total = (len(bins) + batch_size - 1) // batch_size
    for idx in range(0, len(bins), batch_size):
        batch_paths = bins[idx : idx + batch_size]
        batch_ctx = []
        for p in batch_paths:
            try:
                batch_ctx.append({
                    "bin_path": str(p),
                    "filename": p.name,
                    "parent_dir": p.parent.name,
                    "file_size": p.stat().st_size,
                    "embedded_strings": extract_strings_regions(p),
                })
            except Exception as e:
                log(f"  skip {p}: {e}", log_path)

        n = idx // batch_size + 1
        if args.dry_run:
            log(f"Pass 1 DRY-RUN batch {n}/{total}: "
                f"{len(batch_ctx)} bins prepped", log_path)
            continue

        try:
            t0 = time.time()
            resp = hermes_call(build_pass1_prompt(batch_ctx), api_key)
            parsed = parse_hermes_array(resp)
            for r in parsed:
                r["_processed_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
                r["_batch"] = n
            results.extend(parsed)
            append_progress(progress_path, "pass1",
                            [str(p) for p in batch_paths])
            log(f"Pass 1 batch {n}/{total} done in {time.time()-t0:.1f}s "
                f"(total {len(results)})", log_path)
            output_path.write_text(json.dumps(results, indent=2))
        except Exception as e:
            log(f"Pass 1 batch {n} FAILED: {e}", log_path)

    log(f"Pass 1 complete: {len(results)} profiles → {output_path}", log_path)


# ─── PASS 2: FAMILY CLUSTERING ─────────────────────────────────────────────

def build_pass2_prompt(family: str, members: list) -> str:
    return (
        f"You are analyzing a cluster of {len(members)} VAG ECU bin "
        f"profiles all classified as ECU family '{family}'. Identify:\n\n"
        "  - Canonical reference dump(s) for each box code in this family\n"
        "  - Twin bins (same SW version, different cal version)\n"
        "  - Version progression / generational lineage\n"
        "  - Suspected corrupted or partial dumps\n"
        "  - Tool-signature patterns within the family\n\n"
        "Return JSON object:\n"
        "  {\n"
        "    \"family\": \"<copy>\",\n"
        "    \"canonical_per_boxcode\": { \"<box_code>\": "
        "\"<bin_path>\" },\n"
        "    \"twin_groups\": [ [ \"<bin_path>\", ... ] ],\n"
        "    \"version_progression\": [ \"<sw_version>\", ... ],\n"
        "    \"corrupted_or_partial\": [ \"<bin_path>\" ],\n"
        "    \"tool_signature_distribution\": { \"<tool>\": <count> },\n"
        "    \"reasoning\": \"<summary>\"\n"
        "  }\n\n"
        "Pure JSON object, no markdown.\n\n"
        "Family members:\n"
        + json.dumps(members, indent=2)
    )


def run_pass2(args, project_root: Path, log_path: Path,
              progress_path: Path, ts: str):
    pass1_glob = sorted((project_root / "tools").glob(
        "hermes_corpus_catalog_*.json"
    ))
    if not pass1_glob:
        log("Pass 2: no Pass 1 output found — run --pass 1 first",
            log_path)
        return
    profiles = json.loads(pass1_glob[-1].read_text())
    log(f"Pass 2: loaded {len(profiles)} profiles from {pass1_glob[-1]}",
        log_path)

    families = defaultdict(list)
    for p in profiles:
        fam = p.get("ecu_family") or "unknown"
        families[fam].append(p)
    log(f"Pass 2: clustered into {len(families)} families", log_path)

    output_path = project_root / CONFIG["output_pass2_template"].format(ts=ts)
    done = load_progress(progress_path, "pass2") if args.resume else set()
    api_key = None if args.dry_run else load_api_key()

    cluster_results = []
    for fam, members in sorted(families.items()):
        if fam in done:
            log(f"  skip family {fam} (already done)", log_path)
            continue
        if args.limit and len(cluster_results) >= args.limit:
            break

        if args.dry_run:
            log(f"Pass 2 DRY-RUN family '{fam}': {len(members)} members",
                log_path)
            continue

        try:
            t0 = time.time()
            resp = hermes_call(build_pass2_prompt(fam, members), api_key)
            start, end = resp.find("{"), resp.rfind("}")
            cluster = json.loads(resp[start : end + 1])
            cluster["_processed_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
            cluster_results.append(cluster)
            append_progress(progress_path, "pass2", [fam])
            log(f"Pass 2 family '{fam}' ({len(members)} members) "
                f"done in {time.time()-t0:.1f}s", log_path)
            output_path.write_text(json.dumps(cluster_results, indent=2))
        except Exception as e:
            log(f"Pass 2 family '{fam}' FAILED: {e}", log_path)

    log(f"Pass 2 complete: {len(cluster_results)} clusters → {output_path}",
        log_path)


# ─── PASS 3: REJECT DEEP-DIVE ──────────────────────────────────────────────

def build_pass3_prompt(batch: list) -> str:
    return (
        "You are analyzing VAG ECU bin dumps that failed the two "
        "documented AES-128 key offset checks (0x18200 plain MG1CS002, "
        "0x600200 IFX). Pre-scanning found candidate high-entropy 16-byte "
        "windows at NON-documented offsets — your job is to assess "
        "which (if any) of those candidates looks like a real key location "
        "based on:\n\n"
        "  - The bin's likely ECU family (inferred from filename / "
        "parent_dir)\n"
        "  - Plausibility of the offset for that family (Bosch ECUs "
        "store keys at family-specific offsets, e.g., MED17 differs "
        "from MDG1)\n"
        "  - Whether the entropy + offset pattern matches known Bosch "
        "key storage conventions\n\n"
        "Output JSON array, one object per input bin:\n"
        "  {\n"
        "    \"bin_path\": \"<copy>\",\n"
        "    \"likely_family\": \"<guess>\",\n"
        "    \"recommended_offset\": <int> or null,\n"
        "    \"recommended_offset_hex\": \"0x...\" or null,\n"
        "    \"recommended_fingerprint\": \"<8 hex>\" or null,\n"
        "    \"confidence\": \"high | medium | low | uncertain\",\n"
        "    \"alternative_offsets\": [ <int>, ... ],\n"
        "    \"reasoning\": \"<one short paragraph>\"\n"
        "  }\n\n"
        "Pure JSON array, no markdown.\n\n"
        "Input:\n" + json.dumps(batch, indent=2)
    )


def run_pass3(args, project_root: Path, log_path: Path,
              progress_path: Path, ts: str):
    proposal_path = project_root / CONFIG["default_proposal_input"]
    if not proposal_path.exists():
        log(f"Pass 3: proposal file not found: {proposal_path}", log_path)
        return
    data = json.loads(proposal_path.read_text())
    if isinstance(data, dict) and "entries" in data:
        proposals = data["entries"]
        log(f"Pass 3: manifest is a dict-wrapper; extracted "
            f"{len(proposals)} entries", log_path)
    elif isinstance(data, list):
        proposals = data
    else:
        log(f"Pass 3 FATAL: unexpected manifest format: "
            f"{type(data).__name__}", log_path)
        return
    rejects = [
        p for p in proposals
        if p.get("verification_status") == "no_candidate_offset"
    ]
    log(f"Pass 3: {len(rejects)} reject bins to deep-dive", log_path)

    done = load_progress(progress_path, "pass3") if args.resume else set()
    if done:
        rejects = [r for r in rejects if r["bin_path"] not in done]
        log(f"Pass 3: resume — {len(rejects)} remaining", log_path)
    if args.limit:
        rejects = rejects[: args.limit]

    api_key = None if args.dry_run else load_api_key()

    output_path = project_root / CONFIG["output_pass3_template"].format(ts=ts)
    results = []
    batch_size = CONFIG["pass3_batch_size"]
    total = (len(rejects) + batch_size - 1) // batch_size
    for idx in range(0, len(rejects), batch_size):
        batch = rejects[idx : idx + batch_size]
        ctx = []
        for r in batch:
            p = Path(r["bin_path"])
            ctx.append({
                "bin_path": r["bin_path"],
                "filename": p.name,
                "parent_dir": p.parent.name if p.exists() else "",
                "file_size": p.stat().st_size if p.exists() else None,
                "candidate_offsets": scan_candidate_key_offsets(p)
                                     if p.exists() else [],
            })

        n = idx // batch_size + 1
        if args.dry_run:
            log(f"Pass 3 DRY-RUN batch {n}/{total}: "
                f"{len(ctx)} bins prepped", log_path)
            continue

        try:
            t0 = time.time()
            resp = hermes_call(build_pass3_prompt(ctx), api_key)
            parsed = parse_hermes_array(resp)
            for r in parsed:
                r["_processed_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
                r["_batch"] = n
            results.extend(parsed)
            append_progress(progress_path, "pass3",
                            [b["bin_path"] for b in batch])
            log(f"Pass 3 batch {n}/{total} done in {time.time()-t0:.1f}s "
                f"(total {len(results)})", log_path)
            output_path.write_text(json.dumps(results, indent=2))
        except Exception as e:
            log(f"Pass 3 batch {n} FAILED: {e}", log_path)

    log(f"Pass 3 complete: {len(results)} reject analyses → {output_path}",
        log_path)


# ─── SUMMARY MARKDOWN ROLLUP ───────────────────────────────────────────────

def write_summary(project_root: Path, ts: str, log_path: Path) -> None:
    summary_path = project_root / CONFIG["output_summary_template"].format(ts=ts)
    sections = ["# Hermes corpus catalog summary\n",
                f"Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}\n"]
    tools_dir = project_root / "tools"
    for label, glob in [
        ("Pass 1 — per-bin profiles", "hermes_corpus_catalog_*.json"),
        ("Pass 2 — family clusters", "hermes_corpus_clusters_*.json"),
        ("Pass 3 — reject deep-dive", "hermes_corpus_rejects_*.json"),
    ]:
        files = sorted(tools_dir.glob(glob))
        if not files:
            sections.append(f"## {label}\n\n(no output yet)\n")
            continue
        latest = files[-1]
        try:
            data = json.loads(latest.read_text())
            sections.append(f"## {label}\n\nFile: `{latest.name}`  \n"
                            f"Entries: **{len(data)}**\n")
        except Exception as e:
            sections.append(f"## {label}\n\n(parse failed: {e})\n")
    summary_path.write_text("\n".join(sections))
    log(f"Summary written → {summary_path}", log_path)


# ─── MAIN ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pass", dest="pass_", default="all",
                        choices=["1", "2", "3", "all"],
                        help="Which pass to run")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--limit", type=int, default=None,
                        help="Limit bins (Pass 1, 3) or families (Pass 2)")
    args = parser.parse_args()

    project_root = Path.cwd()
    log_path = project_root / CONFIG["log_file"]
    progress_path = project_root / CONFIG["progress_file"]
    ts = time.strftime("%Y%m%d_%H%M%S")

    log(f"=== hermes_corpus_catalog starting (pass={args.pass_}, "
        f"dry_run={args.dry_run}, resume={args.resume}, "
        f"limit={args.limit}) ===", log_path)

    if args.pass_ in ("1", "all"):
        run_pass1(args, project_root, log_path, progress_path, ts)
    if args.pass_ in ("2", "all"):
        run_pass2(args, project_root, log_path, progress_path, ts)
    if args.pass_ in ("3", "all"):
        run_pass3(args, project_root, log_path, progress_path, ts)

    write_summary(project_root, ts, log_path)
    log("=== hermes_corpus_catalog finished ===", log_path)


if __name__ == "__main__":
    main()
