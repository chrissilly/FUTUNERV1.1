#!/usr/bin/env python3
"""bundle_ui.py — concatenate the split UI sources into a single
self-contained HTML file for the dongle's flash partition.

The dongle has no separate static-asset server; the partition serves
exactly one file (`firmware/futuner_control_panel.html`). This script
inlines `ui/control_panel.css` (into a `<style>` tag, replacing the
canonical's `<link rel="stylesheet" ...>`) and `ui/control_panel.js`
(into a `<script>` tag, replacing the canonical's `<script src="..."></script>`).

DETERMINISM CONTRACT — load-bearing:
  - Same input bytes → same output bytes, byte-for-byte.
  - No build timestamps. No random IDs. No environment-dependent
    formatting. The eval re-runs this script and diffs the output.
  - LF line endings only. UTF-8.

Usage:
    python3 tools/bundle_ui.py \\
        --in ui/control_panel.html ui/control_panel.css ui/control_panel.js \\
        --out firmware/futuner_control_panel.html

The order of --in arguments is fixed: html, css, js.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


LINK_CSS_LITERAL = '<link rel="stylesheet" href="control_panel.css">'
SCRIPT_JS_LITERAL = '<script src="control_panel.js"></script>'


def read_text(path: Path) -> str:
    """Read a UTF-8 file and normalise line endings to LF."""
    raw = path.read_bytes()
    # Decode UTF-8 strictly — any invalid byte indicates corruption.
    text = raw.decode("utf-8")
    # Normalise line endings: CRLF and CR → LF. Bundle output is
    # always LF; canonical inputs should already be LF, but normalising
    # here means an editor that snuck CRLF in won't break determinism
    # for downstream consumers.
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return text


def bundle(html_path: Path, css_path: Path, js_path: Path) -> str:
    html = read_text(html_path)
    css = read_text(css_path).rstrip("\n")
    js = read_text(js_path).rstrip("\n")

    if LINK_CSS_LITERAL not in html:
        raise SystemExit(
            f"ERROR: bundle expected to find this exact line in {html_path}:\n"
            f"  {LINK_CSS_LITERAL}\n"
            "If the canonical HTML's CSS reference changed, update "
            "LINK_CSS_LITERAL in tools/bundle_ui.py and re-derive the "
            "determinism contract."
        )
    if SCRIPT_JS_LITERAL not in html:
        raise SystemExit(
            f"ERROR: bundle expected to find this exact line in {html_path}:\n"
            f"  {SCRIPT_JS_LITERAL}\n"
            "If the canonical HTML's JS reference changed, update "
            "SCRIPT_JS_LITERAL in tools/bundle_ui.py."
        )

    inlined_css = "<style>\n" + css + "\n</style>"
    inlined_js = "<script>\n" + js + "\n</script>"

    out = html.replace(LINK_CSS_LITERAL, inlined_css, 1)
    out = out.replace(SCRIPT_JS_LITERAL, inlined_js, 1)

    # Ensure trailing newline — POSIX text-file convention. Idempotent.
    if not out.endswith("\n"):
        out = out + "\n"
    return out


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(
        description=(
            "Bundle the split UI sources (html + css + js) into a single "
            "self-contained HTML file. Deterministic — same inputs always "
            "produce the same output bytes."
        )
    )
    p.add_argument(
        "--in",
        dest="inputs",
        nargs=3,
        required=True,
        metavar=("HTML", "CSS", "JS"),
        help="Input files in fixed order: html, css, js.",
    )
    p.add_argument(
        "--out",
        dest="output",
        required=True,
        help="Path to write the bundled single-file HTML.",
    )
    args = p.parse_args(argv)

    html_path = Path(args.inputs[0]).resolve()
    css_path = Path(args.inputs[1]).resolve()
    js_path = Path(args.inputs[2]).resolve()
    out_path = Path(args.output).resolve()

    for label, path in (("html", html_path), ("css", css_path), ("js", js_path)):
        if not path.is_file():
            raise SystemExit(f"ERROR: {label} input not found: {path}")

    bundled = bundle(html_path, css_path, js_path)
    out_path.write_bytes(bundled.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
