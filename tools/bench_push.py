#!/usr/bin/env python3
"""bench_push.py — phase-gated bench-day push pipeline orchestrator.

Single CLI that unifies the four manual steps documented in
`docs/upload2server.md` (build / flash / push cloud assets / provision
the dongle) into one invocation. Pure dev tool — no firmware C
changes, no feature work, no new endpoints.

Usage (one-liner bench loop):

    ADMIN_API_KEY='...' tools/bench_push.py --all \\
        --port /dev/cu.usbmodem1101 \\
        --sbf-file sbf/stage1_patched.sbf \\
        --paid 1

Phase breakdown (each opt-in via --no-X / --only=X):

    --build       firmware/build.sh (which itself bundles the UI)
    --flash       firmware/flash.sh + post-flash boot-log MAC scrape
    --cloud       PUT /admin/firmware, POST /admin/calibrations,
                  POST /admin/devices (409 → fall through to GET)
    --provision   WS set_auth_token, optional wifi_connect, vin_pair_now,
                  POST /admin/devices/<mac>/license + final vin_pair_now

See tools/README.md for deps. See docs/upload2server.md for what each
phase corresponds to in the manual procedure.

Hard rules carry through (CLAUDE.md):
  - No magic numbers — every default lives in CONFIG below with a
    "Proposed default — needs Sean's approval before lock." annotation.
  - ON/OFF discipline through feature_manager: this script consumes
    WS commands that already arbitrate via feature_manager. It does
    not bypass.
  - Mandatory progress logging: every non-dry-run appends a
    timestamped block to ~/esp/obd/file-update-YYYY-MM-DD.md.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as _dt
import enum
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Optional


# =========================================================================
# CONFIG — every CLI default named, with comments + approval annotation.
# Move literals here; do not add inline magic numbers below.
# =========================================================================
CONFIG: dict[str, Any] = {
    # Cloud base URL. Override with $CLOUD_URL.
    # Proposed default — needs Sean's approval before lock.
    "CLOUD_URL_DEFAULT":          "https://sillyrabbitmotorsport.com/fut",

    # Dongle WS host during provision (AP-mode IP after fresh flash).
    # Proposed default — needs Sean's approval before lock.
    "DONGLE_HOST_DEFAULT":        "ws://192.168.4.1/ws",

    # Boot-log MAC scrape window (seconds) AND the regex pattern.
    # ESP32-S3 prints the MAC line during early boot; 30 s gives the
    # bootloader + app banner plenty of time on a slow flash.
    # Proposed default — needs Sean's approval before lock.
    "MAC_SCRAPE_TIMEOUT_SEC":     30.0,
    "MAC_SCRAPE_BAUD":            115200,
    "MAC_REGEX":                  r"\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\b",

    # Default --paid value if the operator didn't pass --paid.
    # Bench-day default is paid=1 because that's the only state in
    # which the license-gated features (live_tune, wot_log_*) work.
    # Proposed default — needs Sean's approval before lock.
    "DEFAULT_PAID":               1,

    # WS connect / message timeouts (seconds).
    # Proposed default — needs Sean's approval before lock.
    "WS_OPEN_TIMEOUT_SEC":        10.0,
    "WS_RECV_TIMEOUT_SEC":        10.0,

    # HTTP request timeout (seconds). Same value covers GET/POST/PUT
    # — server-side handlers are all sub-second; 30 s is the
    # network-failure deadline, not the happy-path latency.
    # Proposed default — needs Sean's approval before lock.
    "HTTP_TIMEOUT_SEC":           30.0,

    # Serial-port glob patterns by host platform.
    # Proposed default — needs Sean's approval before lock.
    "SERIAL_GLOB_DARWIN":         "/dev/cu.usbmodem*",
    "SERIAL_GLOB_LINUX":          "/dev/ttyUSB*",

    # Workspace-root status logs (hard rule #4 — mandatory).
    # Proposed default — needs Sean's approval before lock.
    "WORKSPACE_LOG_ROOT":         "~/esp/obd",
}


# =========================================================================
# Result types
# =========================================================================
class Status(enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


@dataclass
class StepResult:
    phase: str
    step: str
    status: Status
    detail: str = ""


# =========================================================================
# Logger — one-line-per-step, summary table at end.
# =========================================================================
class StepLogger:
    """Captures step results AND prints them as they happen.

    Tests use this to assert on the rendered summary. Real runs use it
    for both the streaming output and the final table.
    """

    def __init__(self, *, dry_run: bool = False, verbose: bool = False):
        self.results: list[StepResult] = []
        self.dry_run = dry_run
        self.verbose = verbose

    def emit(self, phase: str, step: str, status: Status, detail: str = "") -> None:
        prefix = "DRY  " if self.dry_run else "     "
        marker = {"PASS": "PASS", "FAIL": "FAIL", "SKIP": "SKIP"}[status.value]
        line = f"{prefix}[{marker}] {phase}/{step}"
        if detail:
            line += f"  -- {detail}"
        print(line, flush=True)
        self.results.append(StepResult(phase, step, status, detail))

    def render_summary(self) -> str:
        if not self.results:
            return "no steps run"
        # Group by phase, count by status.
        by_phase: dict[str, dict[str, int]] = {}
        for r in self.results:
            ph = by_phase.setdefault(r.phase, {"PASS": 0, "FAIL": 0, "SKIP": 0})
            ph[r.status.value] += 1
        lines = ["", "=" * 62, "  Summary", "=" * 62]
        for phase, counts in by_phase.items():
            lines.append(
                f"  {phase:<14} PASS={counts['PASS']:<3} "
                f"FAIL={counts['FAIL']:<3} SKIP={counts['SKIP']:<3}"
            )
        total_fail = sum(1 for r in self.results if r.status is Status.FAIL)
        lines.append("")
        lines.append("RESULT: " + ("FAIL" if total_fail else "PASS"))
        return "\n".join(lines)

    def any_failed(self) -> bool:
        return any(r.status is Status.FAIL for r in self.results)


# =========================================================================
# Helpers — exposed at module scope so tests can call them directly.
# =========================================================================
def redact_secret(value: Optional[str], keep_tail: int = 4) -> str:
    """Show only the last `keep_tail` chars; rest as asterisks."""
    if not value:
        return "(unset)"
    if len(value) <= keep_tail:
        return "*" * len(value)
    return ("*" * (len(value) - keep_tail)) + value[-keep_tail:]


def scrape_mac_from_lines(
    lines: Iterable[str],
    mac_regex: str = CONFIG["MAC_REGEX"],
) -> Optional[str]:
    """Pure logic for boot-log MAC scrape — given an iterable of lines,
    return the first MAC address that matches.

    Looks for the canonical NVS line first ("device MAC: XX:..."),
    falls back to any standalone MAC token. The fall-back is what
    makes this resilient to log-format drift; if Sean reflows the
    boot-log message, the fallback pattern still catches the MAC.
    """
    pat = re.compile(mac_regex)
    nvs_pat = re.compile(r"device\s+MAC[: ]\s*" + mac_regex, re.IGNORECASE)
    for line in lines:
        m = nvs_pat.search(line)
        if m:
            return m.group(1).upper()
    # Second pass: any MAC token anywhere.
    for line in lines:
        m = pat.search(line)
        if m:
            return m.group(1).upper()
    return None


def read_serial_lines(
    port: str,
    timeout_sec: float,
    baud: int,
    serial_module: Any = None,
) -> list[str]:
    """Read from a serial port for `timeout_sec` seconds, return
    every newline-terminated chunk decoded as latin-1 (any byte is
    valid latin-1 — never crashes on non-ASCII boot output).

    `serial_module` is injectable for tests. In production it lazily
    imports pyserial.
    """
    if serial_module is None:
        try:
            import serial as serial_module  # type: ignore
        except ImportError as e:  # pragma: no cover
            raise SystemExit(
                "pyserial required for boot-log MAC scrape "
                "(pip install --user pyserial)"
            ) from e

    import time as _time
    deadline = _time.monotonic() + timeout_sec
    s = serial_module.Serial(port, baud, timeout=0.2)
    out: list[str] = []
    buf = bytearray()
    try:
        while _time.monotonic() < deadline:
            chunk = s.read(4096)
            if chunk:
                buf.extend(chunk)
                while b"\n" in buf:
                    line, _, rest = buf.partition(b"\n")
                    out.append(line.decode("latin-1"))
                    buf = bytearray(rest)
                # Quick exit: if we already have a MAC, stop early.
                if scrape_mac_from_lines(out) is not None:
                    break
    finally:
        try:
            s.close()
        except Exception:
            pass
    if buf:
        out.append(bytes(buf).decode("latin-1"))
    return out


def detect_serial_port(plat: str = sys.platform) -> Optional[str]:
    """Look for a single matching serial device; return None if zero
    or multiple match (caller decides what to do)."""
    pattern_key = "SERIAL_GLOB_DARWIN" if plat == "darwin" else "SERIAL_GLOB_LINUX"
    pattern = CONFIG[pattern_key]
    from glob import glob
    matches = sorted(glob(pattern))
    if len(matches) == 1:
        return matches[0]
    return None


def git_dirty(project_root: Path,
              run: Callable[..., subprocess.CompletedProcess] = subprocess.run
              ) -> Optional[str]:
    """Returns a non-empty diagnostic string if the tree is dirty; None
    if clean (or git unavailable, in which case we treat as clean
    and let the operator deal — `git` missing in the dev box is rare
    enough that an extra error message would be noise)."""
    if not (project_root / ".git").exists():
        return None
    try:
        cp = run(
            ["git", "-C", str(project_root), "status", "--porcelain"],
            capture_output=True, text=True, check=False,
        )
    except FileNotFoundError:
        return None
    if cp.returncode != 0:
        return None
    out = (cp.stdout or "").strip()
    if not out:
        return None
    # Return a short summary — first 5 modified files, count.
    lines = out.splitlines()
    summary = lines[:5]
    if len(lines) > 5:
        summary.append(f"... ({len(lines)-5} more)")
    return "\n        ".join(summary)


# =========================================================================
# HTTP client — wraps urllib for testability.
# =========================================================================
class HttpClient:
    """Thin wrapper around urllib.request. Tests inject a fake; in dry
    run, every call short-circuits to the would-have-sent log."""

    def __init__(
        self,
        *,
        admin_key: str,
        cloud_url: str,
        timeout_sec: float,
        dry_run: bool,
        opener: Callable[..., Any] = urllib.request.urlopen,
        logger: Optional[StepLogger] = None,
    ):
        self.admin_key = admin_key
        self.cloud_url = cloud_url.rstrip("/")
        self.timeout = timeout_sec
        self.dry_run = dry_run
        self.opener = opener
        self.logger = logger

    def _build_request(
        self,
        method: str,
        path: str,
        json_body: Optional[dict[str, Any]] = None,
        query: Optional[dict[str, str]] = None,
        multipart: Optional[tuple[str, str, bytes, str]] = None,  # (field, filename, body, content_type)
    ) -> urllib.request.Request:
        url = f"{self.cloud_url}{path}"
        if query:
            url += "?" + urllib.parse.urlencode(query)
        headers = {"x-admin-key": self.admin_key}
        body: Optional[bytes] = None
        if json_body is not None:
            body = json.dumps(json_body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        elif multipart is not None:
            field, filename, fbody, content_type = multipart
            boundary = "----benchpush-boundary"
            crlf = b"\r\n"
            parts = [
                f"--{boundary}".encode("ascii"), crlf,
                f'Content-Disposition: form-data; name="{field}"; filename="{filename}"'.encode("ascii"), crlf,
                f"Content-Type: {content_type}".encode("ascii"), crlf, crlf,
                fbody, crlf,
                f"--{boundary}--".encode("ascii"), crlf,
            ]
            body = b"".join(parts)
            headers["Content-Type"] = f"multipart/form-data; boundary={boundary}"
        req = urllib.request.Request(url, data=body, method=method, headers=headers)
        return req

    def request(
        self,
        method: str,
        path: str,
        *,
        json_body: Optional[dict[str, Any]] = None,
        query: Optional[dict[str, str]] = None,
        multipart: Optional[tuple[str, str, bytes, str]] = None,
        ok_codes: tuple[int, ...] = (200,),
    ) -> tuple[int, dict[str, Any]]:
        """Execute the request. Returns (status_code, parsed_json_or_dict).
        On dry-run, prints the would-be request and returns (200, {})."""
        url = f"{self.cloud_url}{path}"
        if query:
            url += "?" + urllib.parse.urlencode(query)
        if self.dry_run:
            redacted_key = redact_secret(self.admin_key)
            blurb = f"{method} {url}  x-admin-key={redacted_key}"
            if json_body is not None:
                blurb += f"  json={json.dumps(json_body)}"
            if multipart is not None:
                blurb += f"  multipart=<{multipart[1]}, {len(multipart[2])} bytes>"
            print(f"DRY  HTTP    {blurb}", flush=True)
            return 200, {}
        req = self._build_request(method, path, json_body=json_body,
                                  query=query, multipart=multipart)
        try:
            with self.opener(req, timeout=self.timeout) as resp:
                code = resp.getcode()
                raw = resp.read()
                try:
                    data = json.loads(raw.decode("utf-8")) if raw else {}
                except (json.JSONDecodeError, UnicodeDecodeError):
                    data = {"_raw": raw[:200]}
                if code not in ok_codes:
                    return code, data
                return code, data
        except urllib.error.HTTPError as e:
            try:
                data = json.loads(e.read().decode("utf-8"))
            except Exception:
                data = {}
            return e.code, data


# =========================================================================
# WS provisioning — uses websockets library; lazy-imported.
# =========================================================================
def ws_send_command(
    url: str,
    command: str,
    params: Optional[dict[str, Any]] = None,
    *,
    open_timeout: float = CONFIG["WS_OPEN_TIMEOUT_SEC"],
    recv_timeout: float = CONFIG["WS_RECV_TIMEOUT_SEC"],
    runner: Optional[Callable[..., dict[str, Any]]] = None,
) -> dict[str, Any]:
    """Send a single WS command, await one response, return the parsed
    JSON. `runner` is injectable for tests."""
    if runner is not None:
        return runner(url=url, command=command, params=params,
                      open_timeout=open_timeout, recv_timeout=recv_timeout)

    try:
        from websockets.asyncio.client import connect as ws_connect  # type: ignore
    except ImportError as e:  # pragma: no cover
        raise SystemExit(
            "websockets package required for provision phase "
            "(pip install --user websockets)"
        ) from e

    async def _run() -> dict[str, Any]:
        async with ws_connect(url, open_timeout=open_timeout) as ws:
            payload: dict[str, Any] = {"command": command}
            if params is not None:
                payload["params"] = params
            await ws.send(json.dumps(payload))
            raw = await asyncio.wait_for(ws.recv(), timeout=recv_timeout)
            try:
                return json.loads(raw)
            except json.JSONDecodeError:
                return {"_raw": str(raw)[:200]}

    return asyncio.run(_run())


# =========================================================================
# Context — collects everything a phase needs.
# =========================================================================
@dataclass
class Context:
    args:           argparse.Namespace
    env:            dict[str, str]
    project_root:   Path
    logger:         StepLogger
    http:           HttpClient
    # Injectable runners — tests override these.
    subprocess_run: Callable[..., subprocess.CompletedProcess] = field(
        default=subprocess.run
    )
    serial_reader:  Callable[..., list[str]] = field(default=read_serial_lines)
    ws_runner:      Optional[Callable[..., dict[str, Any]]] = None
    confirm:        Callable[[str], bool] = field(default=lambda prompt: True)

    # Mutable state populated by phases as they run.
    scraped_mac:    Optional[str] = None
    auth_token:     Optional[str] = None
    firmware_hash:  Optional[str] = None


# =========================================================================
# Phase: build
# =========================================================================
def phase_build(ctx: Context) -> None:
    phase = "build"
    build_sh = ctx.project_root / "firmware" / "build.sh"
    if not build_sh.is_file():
        ctx.logger.emit(phase, "build.sh", Status.FAIL,
                        f"missing: {build_sh}")
        return
    if ctx.args.dry_run:
        ctx.logger.emit(phase, "build.sh", Status.PASS,
                        f"would run: {build_sh}")
        return
    cp = ctx.subprocess_run([str(build_sh)],
                            capture_output=True, text=True, check=False)
    if cp.returncode == 0:
        ctx.logger.emit(phase, "build.sh", Status.PASS,
                        "idf.py build exited 0")
        if ctx.args.verbose:
            print((cp.stdout or "").strip()[-1000:])
    else:
        tail = (cp.stdout or "")[-400:] + (cp.stderr or "")[-400:]
        ctx.logger.emit(phase, "build.sh", Status.FAIL,
                        f"rc={cp.returncode} ...{tail.strip()[-200:]}")


# =========================================================================
# Phase: flash (+ post-flash MAC scrape)
# =========================================================================
def phase_flash(ctx: Context) -> None:
    phase = "flash"
    flash_sh = ctx.project_root / "firmware" / "flash.sh"
    if not flash_sh.is_file():
        ctx.logger.emit(phase, "flash.sh", Status.FAIL,
                        f"missing: {flash_sh}")
        return

    port = ctx.args.port
    if not port:
        port = detect_serial_port()
        if not port:
            ctx.logger.emit(phase, "port-autodetect", Status.SKIP,
                            "no /dev/cu.usbmodem* (or /dev/ttyUSB*) "
                            "matched — pass --port explicitly")
            return
        ctx.logger.emit(phase, "port-autodetect", Status.PASS, port)

    if ctx.args.dry_run:
        ctx.logger.emit(phase, "flash.sh", Status.PASS,
                        f"would run: {flash_sh} {port}")
        # Carry --mac (if any) through dry-run so subsequent phases can
        # show the full pipeline. If neither --mac nor a scraped value
        # is available, use a placeholder marker.
        ctx.scraped_mac = (ctx.args.mac or "DR:YR:UN:00:00:00").upper()
        ctx.logger.emit(phase, "mac-scrape", Status.PASS,
                        f"would scrape MAC from {port} for "
                        f"{CONFIG['MAC_SCRAPE_TIMEOUT_SEC']}s "
                        f"(dry-run mac={ctx.scraped_mac})")
        return

    cp = ctx.subprocess_run([str(flash_sh), port],
                            capture_output=True, text=True, check=False)
    if cp.returncode != 0:
        tail = (cp.stdout or "")[-400:] + (cp.stderr or "")[-400:]
        ctx.logger.emit(phase, "flash.sh", Status.FAIL,
                        f"rc={cp.returncode} ...{tail.strip()[-200:]}")
        return
    ctx.logger.emit(phase, "flash.sh", Status.PASS,
                    f"esptool wrote app slot 0 to {port}")

    # Post-flash MAC scrape.
    lines = ctx.serial_reader(
        port=port,
        timeout_sec=CONFIG["MAC_SCRAPE_TIMEOUT_SEC"],
        baud=CONFIG["MAC_SCRAPE_BAUD"],
    )
    mac = scrape_mac_from_lines(lines)
    if mac is None:
        ctx.logger.emit(phase, "mac-scrape", Status.FAIL,
                        f"no MAC line in {len(lines)} log lines — pass --mac explicitly")
        return
    ctx.scraped_mac = mac
    ctx.logger.emit(phase, "mac-scrape", Status.PASS, mac)
    # Reconcile against --mac if provided.
    if ctx.args.mac:
        want = ctx.args.mac.upper()
        if want != mac:
            ctx.logger.emit(phase, "mac-reconcile", Status.FAIL,
                            f"flag --mac={want} but boot log shows {mac}")
        else:
            ctx.logger.emit(phase, "mac-reconcile", Status.PASS, "match")


# =========================================================================
# Phase: cloud — push assets and ensure device row exists.
# =========================================================================
def phase_cloud(ctx: Context) -> None:
    phase = "cloud"

    # Resolve the dongle MAC for this run.
    mac = (ctx.args.mac or ctx.scraped_mac or "").upper()
    if not mac:
        ctx.logger.emit(phase, "mac-required", Status.FAIL,
                        "no --mac and no scraped MAC — pass --mac or run --flash first")
        return

    # Step: enroll device (POST /admin/devices). 409 → fall through.
    code, data = ctx.http.request(
        "POST", "/admin/devices",
        json_body={"mac": mac, "vin": ctx.args.vin},
        ok_codes=(200, 409),
    )
    if ctx.args.dry_run:
        ctx.auth_token = ctx.auth_token or "DRYRUN-token-placeholder"
        ctx.logger.emit(phase, "enroll", Status.PASS,
                        f"would POST /admin/devices, token={redact_secret(ctx.auth_token)}")
    elif code == 200 and data.get("ok"):
        ctx.auth_token = data.get("auth_token") or ctx.auth_token
        ctx.logger.emit(phase, "enroll", Status.PASS, f"new device, token={redact_secret(ctx.auth_token)}")
    elif code == 409:
        # Already enrolled — fetch the existing token.
        code2, data2 = ctx.http.request("GET", "/admin/devices")
        if code2 == 200 and isinstance(data2, list):
            row = next((d for d in data2 if (d.get("mac") or "").upper() == mac), None)
            if row and row.get("auth_token"):
                ctx.auth_token = row["auth_token"]
                ctx.logger.emit(phase, "enroll", Status.SKIP,
                                f"already enrolled, reusing token={redact_secret(ctx.auth_token)}")
            else:
                ctx.logger.emit(phase, "enroll", Status.FAIL,
                                f"409 on POST but device {mac} not in /admin/devices listing")
                return
        elif ctx.args.dry_run:
            # Dry run: pretend.
            ctx.logger.emit(phase, "enroll", Status.SKIP, "would re-enroll, fetch token via GET /admin/devices")
        else:
            ctx.logger.emit(phase, "enroll", Status.FAIL,
                            f"GET /admin/devices failed: code={code2}")
            return
    else:
        ctx.logger.emit(phase, "enroll", Status.FAIL,
                        f"POST /admin/devices code={code} body={data}")
        return

    # Step: SBF upload (optional).
    if ctx.args.sbf_file:
        sbf_path = Path(ctx.args.sbf_file).expanduser()
        if not sbf_path.is_file() and not ctx.args.dry_run:
            ctx.logger.emit(phase, "sbf-upload", Status.FAIL, f"missing: {sbf_path}")
            return
        body = sbf_path.read_bytes() if sbf_path.is_file() else b""
        params = {"boxcode": ctx.args.boxcode} if ctx.args.boxcode else None
        code, data = ctx.http.request(
            "POST", f"/admin/calibrations/{sbf_path.name}",
            multipart=("file", sbf_path.name, body, "application/octet-stream"),
            query=params,
        )
        if code == 200 and data.get("ok"):
            ctx.logger.emit(phase, "sbf-upload", Status.PASS,
                            f"{sbf_path.name} ({data.get('size_bytes', 0)} bytes)")
            # Assign.
            code2, data2 = ctx.http.request(
                "POST", f"/admin/devices/{mac}/assign_calibration",
                query={"filename": sbf_path.name},
            )
            if code2 == 200 and data2.get("ok"):
                ctx.logger.emit(phase, "sbf-assign", Status.PASS, sbf_path.name)
            else:
                ctx.logger.emit(phase, "sbf-assign", Status.FAIL,
                                f"code={code2} body={data2}")
        elif ctx.args.dry_run:
            ctx.logger.emit(phase, "sbf-upload", Status.PASS, f"would upload + assign {sbf_path.name}")
            ctx.logger.emit(phase, "sbf-assign", Status.PASS, f"would assign {sbf_path.name} → {mac}")
        else:
            ctx.logger.emit(phase, "sbf-upload", Status.FAIL,
                            f"code={code} body={data}")
            return
    else:
        ctx.logger.emit(phase, "sbf-upload", Status.SKIP, "no --sbf-file")

    # Step: firmware upload (optional, only if a build artifact is on disk).
    fw_bin = ctx.project_root / "firmware" / "build" / "futuner_v2.bin"
    if fw_bin.is_file() or ctx.args.dry_run:
        # Use git short-hash as build_hash. Same convention as upload2server.md.
        # Skip the git subprocess call entirely in dry-run — keeps the
        # mode hermetic (zero side effects, including read-only git).
        if ctx.args.dry_run:
            build_hash = ctx.firmware_hash or "dryrun"
        else:
            build_hash = ctx.firmware_hash or _git_short_hash(
                ctx.project_root, ctx.subprocess_run
            ) or "unknown"
        ctx.firmware_hash = build_hash
        body = fw_bin.read_bytes() if fw_bin.is_file() else b""
        code, data = ctx.http.request(
            "PUT", f"/admin/firmware/{build_hash}",
            multipart=("file", "futuner_v2.bin", body, "application/octet-stream"),
            query={"build_number": "1", "is_release": "1"},
        )
        if code == 200 and data.get("ok"):
            ctx.logger.emit(phase, "fw-upload", Status.PASS,
                            f"build_hash={build_hash} size={data.get('size_bytes', 0)}")
            # Assign.
            code2, data2 = ctx.http.request(
                "POST", f"/admin/devices/{mac}/assign_firmware",
                query={"build_hash": build_hash},
            )
            if code2 == 200 and data2.get("ok"):
                ctx.logger.emit(phase, "fw-assign", Status.PASS, build_hash)
            else:
                ctx.logger.emit(phase, "fw-assign", Status.FAIL,
                                f"code={code2} body={data2}")
        elif ctx.args.dry_run:
            ctx.logger.emit(phase, "fw-upload", Status.PASS,
                            f"would upload + assign build_hash={build_hash}")
            ctx.logger.emit(phase, "fw-assign", Status.PASS, f"would assign {build_hash} → {mac}")
        else:
            ctx.logger.emit(phase, "fw-upload", Status.FAIL,
                            f"code={code} body={data}")
    else:
        ctx.logger.emit(phase, "fw-upload", Status.SKIP,
                        "no firmware/build/futuner_v2.bin (run --build first)")

    # Step: license set (idempotent — server INSERT OR REPLACE-style UPDATE).
    paid = ctx.args.paid
    if paid is None:
        paid = CONFIG["DEFAULT_PAID"]
    code, data = ctx.http.request(
        "POST", f"/admin/devices/{mac}/license",
        json_body={"paid": int(paid)},
    )
    if code == 200 and data.get("ok"):
        ctx.logger.emit(phase, "license-set", Status.PASS, f"paid={paid}")
    elif ctx.args.dry_run:
        ctx.logger.emit(phase, "license-set", Status.PASS, f"would set paid={paid}")
    else:
        ctx.logger.emit(phase, "license-set", Status.FAIL,
                        f"code={code} body={data}")


def _git_short_hash(project_root: Path,
                    run: Callable[..., subprocess.CompletedProcess]) -> Optional[str]:
    if not (project_root / ".git").exists():
        return None
    try:
        cp = run(["git", "-C", str(project_root), "rev-parse", "--short", "HEAD"],
                 capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return None
    if cp.returncode != 0:
        return None
    return (cp.stdout or "").strip() or None


# =========================================================================
# Phase: provision — WS-side dongle setup.
# =========================================================================
def phase_provision(ctx: Context) -> None:
    phase = "provision"

    if not ctx.auth_token:
        ctx.logger.emit(phase, "auth-token", Status.FAIL,
                        "no auth_token — cloud phase must run first OR pass --auth-token")
        return
    if not (ctx.args.flash or ctx.args.i_know_what_im_doing):
        ctx.logger.emit(phase, "flash-coupling", Status.FAIL,
                        "provisioning without --flash is dangerous; "
                        "pass --i-know-what-im-doing to override")
        return

    irreversible_steps = ["set_auth_token", "wifi_connect"]
    for s in irreversible_steps:
        if not ctx.args.yes and not ctx.args.dry_run:
            ok = ctx.confirm(f"Provision: about to send {s} to {ctx.args.dongle_host} — proceed?")
            if not ok:
                ctx.logger.emit(phase, s, Status.SKIP, "operator declined")
                return

    # Step: set_auth_token.
    if ctx.args.dry_run:
        ctx.logger.emit(phase, "set_auth_token", Status.PASS,
                        f"would WS-send to {ctx.args.dongle_host} "
                        f"token={redact_secret(ctx.auth_token)}")
    else:
        try:
            resp = ws_send_command(
                ctx.args.dongle_host, "set_auth_token",
                params={"token": ctx.auth_token},
                runner=ctx.ws_runner,
            )
            if resp.get("ok") or resp.get("success"):
                ctx.logger.emit(phase, "set_auth_token", Status.PASS, "dongle accepted token")
            else:
                ctx.logger.emit(phase, "set_auth_token", Status.FAIL,
                                f"resp={resp}")
                return
        except Exception as e:
            ctx.logger.emit(phase, "set_auth_token", Status.FAIL, f"WS error: {e}")
            return

    # Step: wifi_connect (optional — only if --ssid passed).
    if ctx.args.ssid:
        if ctx.args.dry_run:
            ctx.logger.emit(phase, "wifi_connect", Status.PASS,
                            f"would WS-send wifi_connect ssid={ctx.args.ssid}")
        else:
            try:
                resp = ws_send_command(
                    ctx.args.dongle_host, "wifi_connect",
                    params={"ssid": ctx.args.ssid, "password": ctx.args.wifi_password or ""},
                    runner=ctx.ws_runner,
                )
                if resp.get("success") or resp.get("ok"):
                    ctx.logger.emit(phase, "wifi_connect", Status.PASS, ctx.args.ssid)
                else:
                    ctx.logger.emit(phase, "wifi_connect", Status.FAIL, f"resp={resp}")
            except Exception as e:
                ctx.logger.emit(phase, "wifi_connect", Status.FAIL, f"WS error: {e}")
    else:
        ctx.logger.emit(phase, "wifi_connect", Status.SKIP, "no --ssid")

    # Step: vin_pair_now (idempotent on cloud side).
    if ctx.args.dry_run:
        ctx.logger.emit(phase, "vin_pair_now", Status.PASS,
                        f"would WS-send vin_pair_now to {ctx.args.dongle_host}")
        return
    try:
        resp = ws_send_command(
            ctx.args.dongle_host, "vin_pair_now",
            runner=ctx.ws_runner,
        )
        if resp.get("ok") or resp.get("success"):
            ctx.logger.emit(phase, "vin_pair_now", Status.PASS,
                            resp.get("message") or "paired")
        else:
            ctx.logger.emit(phase, "vin_pair_now", Status.FAIL, f"resp={resp}")
    except Exception as e:
        ctx.logger.emit(phase, "vin_pair_now", Status.FAIL, f"WS error: {e}")


# =========================================================================
# Pipeline orchestration
# =========================================================================
PHASES: dict[str, Callable[[Context], None]] = {
    "build":     phase_build,
    "flash":     phase_flash,
    "cloud":     phase_cloud,
    "provision": phase_provision,
}


def select_phases(args: argparse.Namespace) -> list[str]:
    if args.only:
        if args.only not in PHASES:
            raise SystemExit(
                f"--only={args.only} unknown; pick one of {list(PHASES)}"
            )
        return [args.only]
    out = []
    if args.build:     out.append("build")
    if args.flash:     out.append("flash")
    if args.cloud:     out.append("cloud")
    if args.provision: out.append("provision")
    return out


def run_pipeline(ctx: Context, phases: list[str]) -> None:
    for ph in phases:
        if ph not in PHASES:
            ctx.logger.emit(ph, "unknown-phase", Status.FAIL,
                            f"phase '{ph}' not in {list(PHASES)}")
            continue
        PHASES[ph](ctx)


# =========================================================================
# Validation — fail fast on missing env / flags.
# =========================================================================
def validate_preconditions(args: argparse.Namespace, env: dict[str, str],
                           selected: list[str], project_root: Path) -> list[str]:
    """Returns a list of human-readable error messages. Empty list = OK."""
    errors: list[str] = []
    needs_admin_key = ("cloud" in selected) or ("provision" in selected)
    if needs_admin_key and not env.get("ADMIN_API_KEY"):
        errors.append(
            "ADMIN_API_KEY not set — required for --cloud / --provision phases"
        )
    if "cloud" in selected and not (args.mac or "flash" in selected):
        errors.append(
            "--cloud requires --mac (or --flash to scrape it from the boot log)"
        )
    if "provision" in selected and not args.flash and not args.i_know_what_im_doing:
        errors.append(
            "--provision requires --flash by default (the dongle's state "
            "is unknown without a fresh flash). Override with "
            "--i-know-what-im-doing if you really mean it."
        )
    if not args.allow_dirty and not args.dry_run and "flash" in selected:
        dirt = git_dirty(project_root)
        if dirt:
            errors.append(
                "git tree is dirty (would lose 'what's on the dongle' "
                "↔ git binding). First 5 entries:\n        " + dirt
                + "\n        Pass --allow-dirty to proceed anyway."
            )
    return errors


# =========================================================================
# Mandatory progress logging — per CLAUDE.md hard rule #4.
# =========================================================================
def append_progress_log(ctx: Context, summary_text: str) -> None:
    if ctx.args.dry_run:
        return
    log_root = Path(os.path.expandvars(os.path.expanduser(CONFIG["WORKSPACE_LOG_ROOT"])))
    today = _dt.date.today().isoformat()
    path = log_root / f"file-update-{today}.md"
    if not log_root.exists():
        return  # no workspace root — skip silently (test env, etc.).
    block = "\n".join([
        "",
        "---",
        "",
        f"## bench_push run @ {_dt.datetime.now().isoformat(timespec='seconds')}",
        "",
        f"Phases requested: `{', '.join(select_phases(ctx.args)) or '(none)'}`",
        f"MAC: `{ctx.args.mac or ctx.scraped_mac or '(unknown)'}`",
        f"Cloud: `{ctx.http.cloud_url}`",
        f"Dongle host: `{ctx.args.dongle_host}`",
        "",
        "```",
        summary_text,
        "```",
        "",
    ])
    with path.open("a", encoding="utf-8") as f:
        f.write(block)


# =========================================================================
# main()
# =========================================================================
def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="bench_push.py",
        description=(
            "Phase-gated bench-day push pipeline. Wraps build → flash → "
            "cloud-push → provision into one orchestrator. Each phase "
            "is opt-out via --no-X or selectable via --only=X."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  ADMIN_API_KEY=xxx bench_push.py --all --port /dev/cu.usbmodem1101\n"
            "  bench_push.py --only=cloud --mac AA:BB:CC:DD:EE:FF "
            "--sbf-file sbf/stage1_patched.sbf\n"
            "  bench_push.py --dry-run --all --mac AA:BB:CC:DD:EE:FF\n"
        ),
    )
    # Phase toggles (default ON; --no-X turns off).
    p.add_argument("--build",     action=argparse.BooleanOptionalAction, default=True,
                   help="run firmware/build.sh (default: on)")
    p.add_argument("--flash",     action=argparse.BooleanOptionalAction, default=True,
                   help="run firmware/flash.sh + scrape MAC from boot log (default: on)")
    p.add_argument("--cloud",     action=argparse.BooleanOptionalAction, default=True,
                   help="push assets to cloud (default: on)")
    p.add_argument("--provision", action=argparse.BooleanOptionalAction, default=True,
                   help="WS-side dongle provision (default: on, requires --flash unless --i-know-what-im-doing)")
    p.add_argument("--all", action="store_true",
                   help="explicit form of all-phases-on (no-op if defaults are kept)")
    p.add_argument("--only", choices=list(PHASES.keys()),
                   help="run exactly one phase, skip the rest")

    # Per-run identity / inputs.
    p.add_argument("--port", help="serial port for flash + boot-log scrape "
                                  "(auto-detected if a single match)")
    p.add_argument("--mac", help="dongle MAC (auto-scraped after --flash; required for --cloud without --flash)")
    p.add_argument("--vin", help="optional VIN to enroll with (otherwise dongle reports it)")
    p.add_argument("--boxcode", help="optional boxcode for SBF upload query string")
    p.add_argument("--sbf-file", help="path to SBF to upload + assign (skipped if omitted)")
    p.add_argument("--paid", type=int, choices=[0, 1],
                   help=f"set device paid flag (default: {CONFIG['DEFAULT_PAID']})")
    p.add_argument("--dongle-host", default=CONFIG["DONGLE_HOST_DEFAULT"],
                   help=f"dongle WS URL (default: {CONFIG['DONGLE_HOST_DEFAULT']})")
    p.add_argument("--ssid", help="optional WiFi SSID to send during provision")
    p.add_argument("--wifi-password", help="WiFi password to send with --ssid")
    p.add_argument("--auth-token",
                   help="paste a previously-issued device token instead of "
                        "running --cloud (advanced; bypasses re-enrollment)")

    # Behavior knobs.
    p.add_argument("--dry-run", action="store_true",
                   help="print every command/HTTP/WS call; no side effects")
    p.add_argument("--verbose", action="store_true",
                   help="more per-step output (full subprocess stdout, etc.)")
    p.add_argument("--allow-dirty", action="store_true",
                   help="proceed even if git tree is dirty (dangerous: loses "
                        "the binding between 'what's on the dongle' and git)")
    p.add_argument("--i-know-what-im-doing", action="store_true",
                   help="override --provision-requires-flash check")
    p.add_argument("--yes", "-y", action="store_true",
                   help="skip operator confirmation prompts before "
                        "irreversible WS steps (set_auth_token, wifi_connect)")
    return p


def main(argv: Optional[list[str]] = None,
         *,
         env: Optional[dict[str, str]] = None,
         project_root: Optional[Path] = None,
         logger: Optional[StepLogger] = None,
         http_client: Optional[HttpClient] = None,
         subprocess_runner: Optional[Callable[..., subprocess.CompletedProcess]] = None,
         serial_reader: Optional[Callable[..., list[str]]] = None,
         ws_runner: Optional[Callable[..., dict[str, Any]]] = None,
         confirm: Optional[Callable[[str], bool]] = None,
         ) -> int:
    parser = build_argparser()
    args = parser.parse_args(argv)
    env = dict(os.environ) if env is None else env

    project_root = project_root or _default_project_root()
    selected = select_phases(args)

    pre_errors = validate_preconditions(args, env, selected, project_root)
    if pre_errors:
        for e in pre_errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 2

    cloud_url = env.get("CLOUD_URL", CONFIG["CLOUD_URL_DEFAULT"])
    admin_key = env.get("ADMIN_API_KEY", "")
    logger = logger or StepLogger(dry_run=args.dry_run, verbose=args.verbose)
    http = http_client or HttpClient(
        admin_key=admin_key,
        cloud_url=cloud_url,
        timeout_sec=CONFIG["HTTP_TIMEOUT_SEC"],
        dry_run=args.dry_run,
        logger=logger,
    )
    ctx = Context(
        args=args,
        env=env,
        project_root=project_root,
        logger=logger,
        http=http,
        subprocess_run=subprocess_runner or subprocess.run,
        serial_reader=serial_reader or read_serial_lines,
        ws_runner=ws_runner,
        confirm=confirm or (lambda prompt: _ask_yes_no(prompt)),
    )
    if args.auth_token:
        ctx.auth_token = args.auth_token

    print(f"bench_push: phases={selected} dry-run={args.dry_run} "
          f"cloud={cloud_url} dongle={args.dongle_host}")
    if args.dry_run:
        print(f"DRY  ADMIN_API_KEY={redact_secret(admin_key)}")

    run_pipeline(ctx, selected)

    summary = logger.render_summary()
    print(summary)
    append_progress_log(ctx, summary)
    return 1 if logger.any_failed() else 0


def _ask_yes_no(prompt: str) -> bool:
    """Default operator-confirm hook — replaceable in tests via the
    Context.confirm field. Reads stdin once, returns True iff the
    response starts with 'y' or 'Y'."""
    try:
        ans = input(prompt + " [y/N] ").strip().lower()
    except EOFError:
        return False
    return ans.startswith("y")


def _default_project_root() -> Path:
    """Walk up from this file until a .git dir or 'firmware/' sibling
    exists — that's the project root regardless of cwd."""
    here = Path(__file__).resolve()
    for p in [here.parent, *here.parents]:
        if (p / "firmware").is_dir() and (p / "ui").is_dir():
            return p
    return here.parent.parent


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
