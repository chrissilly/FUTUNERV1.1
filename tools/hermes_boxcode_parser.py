#!/usr/bin/env python3
"""
hermes_boxcode_parser.py

Fire-and-forget script: for each bin in the proposed-merge JSON
that has no parsed box code, ask Hermes (Nemotron) to infer the
box code from filename context + embedded strings inside the bin.

Designed to run unattended in a tmux/screen session while other
work (HIL preflight, etc.) happens elsewhere.

Inputs:
  - tools/proposed_manifest_merge_2026-05-12.json (or --input override)
  - $HERMES_BASE_URL (default: http://192.168.1.180:3000/api)
  - $HERMES_API_KEY or OPENAI_API_KEY (loaded from ~/.hermes/.env if not set)

Outputs:
  - tools/hermes_boxcode_parsed_<ts>.json   (results, incrementally written)
  - tools/hermes_boxcode_parser.progress    (resumable on restart)
  - tools/hermes_boxcode_parser.log         (per-run log)

Usage:
  python3 tools/hermes_boxcode_parser.py            # run with defaults
  python3 tools/hermes_boxcode_parser.py --dry-run  # parse inputs, no API call
  python3 tools/hermes_boxcode_parser.py --batch 25 # smaller batches
  python3 tools/hermes_boxcode_parser.py --resume   # continue from progress file
  python3 tools/hermes_boxcode_parser.py --limit 5  # testing — first N only
"""

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

try:
    import requests
except ImportError:
    print("Install: pip3 install requests", file=sys.stderr)
    sys.exit(1)


# ─── CONFIG ────────────────────────────────────────────────────────────────
# Adjust if your endpoint or model differs. Items marked "approval" should
# be reviewed by Sean before being baked in long-term.

CONFIG = {
    # Hermes / Nemotron endpoint and auth
    "hermes_base_url": os.environ.get(
        "HERMES_BASE_URL", "http://192.168.1.180:3000/api"
    ),
    "hermes_model": os.environ.get("HERMES_MODEL", "nemo180:latest"),
    "hermes_env_file": os.path.expanduser("~/.hermes/.env"),
    "request_timeout_sec": 120,
    "max_retries_per_batch": 3,
    "retry_backoff_sec": 5,

    # Batching — needs Sean's approval before lock-in
    "default_batch_size": 25,

    # Regions of the bin to scan for embedded ASCII strings.
    # Each (offset, length). These are the typical Bosch MDG1 metadata
    # zones; needs Sean's approval before lock-in.
    "bin_metadata_regions": [
        (0x000000, 0x10000),   # first 64 KB — file header / nameplate region
        (0x017000, 0x02000),   # around plain MG1CS002 key offset
        (0x600000, 0x02000),   # around IFX key offset (where boxcode often appears)
    ],
    "min_ascii_run_length": 6,
    "max_strings_per_region": 20,

    # VAG part-number regex — used to pre-rank embedded strings
    "vag_partno_pattern": re.compile(
        r"\b[0-9][A-Z0-9][0-9][A-Z0-9]\d{6}[A-Z]?(?:\s+\d{3,4})?\b"
    ),

    # I/O paths (relative to FUTV1.1/)
    "default_input": "tools/proposed_manifest_merge_2026-05-12.json",
    "output_template": "tools/hermes_boxcode_parsed_{ts}.json",
    "progress_file": "tools/hermes_boxcode_parser.progress",
    "log_file": "tools/hermes_boxcode_parser.log",
}


# ─── HELPERS ───────────────────────────────────────────────────────────────

def load_api_key() -> str:
    """Load Hermes API key from env or ~/.hermes/.env."""
    key = os.environ.get("HERMES_API_KEY") or os.environ.get("OPENAI_API_KEY")
    if key:
        return key
    env_path = Path(CONFIG["hermes_env_file"])
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            line = line.strip()
            if line.startswith("OPENAI_API_KEY=") or line.startswith("HERMES_API_KEY="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise RuntimeError(
        "No API key found. Set HERMES_API_KEY (or OPENAI_API_KEY) in env "
        "or in ~/.hermes/.env"
    )


def extract_strings(path: Path) -> dict:
    """Extract printable-ASCII string runs from configured bin regions.
    Returns {region_offset_hex: [string, ...]}, ranked with VAG-pattern
    matches first."""
    pattern_run = re.compile(
        rb"[\x20-\x7E]{%d,}" % CONFIG["min_ascii_run_length"]
    )
    result = {}
    try:
        size = path.stat().st_size
        with open(path, "rb") as f:
            for offset, length in CONFIG["bin_metadata_regions"]:
                if offset >= size:
                    continue
                f.seek(offset)
                chunk = f.read(min(length, size - offset))
                runs = pattern_run.findall(chunk)
                ranked = sorted(
                    set(r.decode("ascii", errors="ignore") for r in runs),
                    key=lambda s: (
                        not bool(CONFIG["vag_partno_pattern"].search(s)),
                        -len(s),
                    ),
                )
                key = f"0x{offset:06X}"
                result[key] = ranked[: CONFIG["max_strings_per_region"]]
    except Exception as e:
        result["_error"] = str(e)
    return result


def build_hermes_batch_prompt(entries: list) -> str:
    """Build the user message for one batch of bins."""
    return (
        "You are interpreting VAG ECU bin metadata to determine the "
        "ECU's box code (part number).\n\n"
        "VAG box codes follow the pattern: digit, alpha, digit, alpha, "
        "6 digits, optional alpha, optional space + 3-4 digit hardware "
        "version. Example: '4K0907557G' or '4K0907557G 0003'.\n\n"
        "For each bin entry below, return your best inference of the "
        "box code based on filename, parent_dir, and embedded_strings. "
        "Embedded strings are organized by the file offset where they "
        "were found. Strings matching the VAG part-number pattern are "
        "listed first within each region.\n\n"
        "If no clean box code can be determined, return null with a "
        "brief note explaining what you saw.\n\n"
        "Output rules:\n"
        "  - Pure JSON array, one object per input entry, same order\n"
        "  - No markdown, no commentary outside the array\n"
        "  - Schema per entry: {\n"
        "      \"bin_path\": \"<copy from input>\",\n"
        "      \"box_code\": \"<inferred>\" or null,\n"
        "      \"confidence\": \"high\" | \"medium\" | \"low\" | \"uncertain\",\n"
        "      \"reasoning\": \"<one short sentence>\"\n"
        "    }\n\n"
        "Input entries:\n"
        + json.dumps(entries, indent=2)
    )


def hermes_call(prompt: str, api_key: str) -> str:
    """POST one batch to Hermes, return the assistant content string."""
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
                url,
                headers=headers,
                json=payload,
                timeout=CONFIG["request_timeout_sec"],
            )
            r.raise_for_status()
            data = r.json()
            return data["choices"][0]["message"]["content"]
        except Exception as e:
            last_err = e
            time.sleep(CONFIG["retry_backoff_sec"] * (attempt + 1))
    raise RuntimeError(f"Hermes call failed after retries: {last_err}")


def parse_hermes_response(text: str) -> list:
    """Pull the JSON array out of Hermes' response.
    Some models wrap JSON in markdown fences or add prose; handle both."""
    try:
        parsed = json.loads(text)
        if isinstance(parsed, list):
            return parsed
    except json.JSONDecodeError:
        pass
    start = text.find("[")
    end = text.rfind("]")
    if start >= 0 and end > start:
        try:
            return json.loads(text[start : end + 1])
        except json.JSONDecodeError:
            pass
    raise ValueError("Hermes returned no parseable JSON array")


def load_progress(progress_path: Path) -> set:
    if not progress_path.exists():
        return set()
    return set(progress_path.read_text().splitlines())


def append_progress(progress_path: Path, paths: list) -> None:
    with open(progress_path, "a") as f:
        for p in paths:
            f.write(p + "\n")


def log(msg: str, log_path: Path) -> None:
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(log_path, "a") as f:
        f.write(line + "\n")


# ─── MAIN ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--input", default=CONFIG["default_input"])
    parser.add_argument("--batch", type=int, default=CONFIG["default_batch_size"])
    parser.add_argument("--dry-run", action="store_true",
                        help="Parse inputs, build prompts, skip API calls")
    parser.add_argument("--resume", action="store_true",
                        help="Skip bins already listed in progress file")
    parser.add_argument("--limit", type=int, default=None,
                        help="Process at most N bins (testing only)")
    args = parser.parse_args()

    project_root = Path.cwd()
    log_path = project_root / CONFIG["log_file"]
    progress_path = project_root / CONFIG["progress_file"]
    ts = time.strftime("%Y%m%d_%H%M%S")
    output_path = project_root / CONFIG["output_template"].format(ts=ts)

    log(
        f"Starting hermes_boxcode_parser "
        f"(input={args.input}, batch={args.batch}, "
        f"dry_run={args.dry_run}, resume={args.resume}, "
        f"limit={args.limit})",
        log_path,
    )

    input_path = project_root / args.input
    if not input_path.exists():
        log(f"FATAL: input file not found: {input_path}", log_path)
        sys.exit(1)
    data = json.loads(input_path.read_text())
    if isinstance(data, dict) and "entries" in data:
        entries = data["entries"]
        log(f"Manifest is a dict-wrapper; extracted {len(entries)} entries "
            f"from .entries key", log_path)
    elif isinstance(data, list):
        entries = data
    else:
        log(f"FATAL: unexpected manifest format at top level: "
            f"{type(data).__name__}", log_path)
        sys.exit(1)
    log(f"Loaded {len(entries)} entries from {input_path}", log_path)

    todo = [
        e for e in entries
        if (e.get("box_code") in (None, "", "null"))
        or (
            isinstance(e.get("notes", ""), str)
            and "no_box_code" in e.get("notes", "")
        )
    ]
    log(f"Filtered to {len(todo)} bins needing box-code inference", log_path)

    done = load_progress(progress_path) if args.resume else set()
    if done:
        before = len(todo)
        todo = [e for e in todo if e["bin_path"] not in done]
        log(f"Resume: {before - len(todo)} already done, "
            f"{len(todo)} remaining", log_path)

    if args.limit:
        todo = todo[: args.limit]
        log(f"--limit applied: {len(todo)} bins to process", log_path)

    if not todo:
        log("Nothing to do. Exiting.", log_path)
        return

    api_key = None
    if not args.dry_run:
        api_key = load_api_key()
        log("API key loaded", log_path)

    all_results = []
    total_batches = (len(todo) + args.batch - 1) // args.batch
    for batch_idx in range(0, len(todo), args.batch):
        batch_num = batch_idx // args.batch + 1
        batch = todo[batch_idx : batch_idx + args.batch]
        batch_prepped = []
        for e in batch:
            p = Path(e["bin_path"])
            ctx = {
                "bin_path": e["bin_path"],
                "filename": p.name,
                "parent_dir": p.parent.name,
                "file_size": e.get("file_size") or (
                    p.stat().st_size if p.exists() else None
                ),
                "embedded_strings": extract_strings(p) if p.exists() else {},
            }
            batch_prepped.append(ctx)

        if args.dry_run:
            log(f"DRY-RUN batch {batch_num}/{total_batches}: "
                f"{len(batch_prepped)} bins prepared (no API call)",
                log_path)
            continue

        prompt = build_hermes_batch_prompt(batch_prepped)
        try:
            t0 = time.time()
            resp = hermes_call(prompt, api_key)
            parsed = parse_hermes_response(resp)
            elapsed = time.time() - t0
        except Exception as e:
            log(f"BATCH {batch_num}/{total_batches} FAILED: {e}", log_path)
            continue

        for r in parsed:
            r["_processed_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
            r["_batch_idx"] = batch_num
        all_results.extend(parsed)
        append_progress(progress_path, [e["bin_path"] for e in batch])
        log(
            f"Batch {batch_num}/{total_batches} done in {elapsed:.1f}s: "
            f"{len(parsed)} entries, total {len(all_results)}",
            log_path,
        )

        output_path.write_text(json.dumps(all_results, indent=2))

    log(f"Complete. Wrote {len(all_results)} entries to {output_path}",
        log_path)


if __name__ == "__main__":
    main()
