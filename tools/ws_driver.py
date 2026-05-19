#!/usr/bin/env python3
"""Minimal WebSocket driver for the dongle's command surface.

Used by the 2026-05-17 Phase 1 smoke test Tier 2 to script-drive the
WS-only commands (`wot_log_start`, `dtc_read`, `vin_pair_now`,
`wifi_status`, `wifi_mode`, etc.) from this PC over the dongle's STA-side
LAN IP. Drives `ws://<dongle>/ws` with the same JSON shape that
`firmware/src/commands/command_handler.c::command_handler_process_message`
expects: `{"command": "<name>", "params": {...}}` and `{"command":
"unlock", "password": "..."}`.

Streams every send + response to stdout for the smoke-test transcript.

Usage:
  python3 tools/ws_driver.py --host 192.168.1.59 \\
      --script unlock futuner_admin_2024 \\
      --script wifi_status \\
      --script dtc_read

Or interactive (one command per line on stdin, EOF to exit):
  python3 tools/ws_driver.py --host 192.168.1.59 --interactive
"""
import argparse
import asyncio
import json
import shlex
import sys
import time

import websockets


def parse_command_line(line):
    """Parse `<cmd>` or `<cmd> <password>` or `<cmd> k1=v1 k2=v2` into a WS message."""
    parts = shlex.split(line)
    if not parts:
        return None
    cmd = parts[0]
    rest = parts[1:]
    if cmd == "unlock":
        if not rest:
            raise ValueError("unlock requires the password as the next token")
        return {"command": "unlock", "password": rest[0]}
    params = {}
    for tok in rest:
        if "=" in tok:
            k, v = tok.split("=", 1)
            # Best-effort type coercion: bool / int / float / string
            if v.lower() in ("true", "false"):
                params[k] = (v.lower() == "true")
            else:
                try:
                    params[k] = int(v)
                except ValueError:
                    try:
                        params[k] = float(v)
                    except ValueError:
                        params[k] = v
        else:
            # Positional args mapped by index for simple commands like
            # `wifi_sta_set <ssid> <password>`.
            params.setdefault("_args", []).append(tok)
    msg = {"command": cmd}
    if params:
        msg["params"] = params
    return msg


async def drive(host, commands, response_timeout=10.0):
    uri = f"ws://{host}/ws"
    print(f"# Connecting to {uri}", file=sys.stderr)
    async with websockets.connect(uri, open_timeout=5) as ws:
        print(f"# Connected", file=sys.stderr)
        for raw in commands:
            raw = raw.strip()
            if not raw or raw.startswith("#"):
                continue
            msg = parse_command_line(raw)
            if msg is None:
                continue
            print(f"\n>>> {raw}")
            print(f"# WS send: {json.dumps(msg)}")
            await ws.send(json.dumps(msg))
            # Read responses until quiet for `response_timeout` seconds.
            deadline = time.time() + response_timeout
            got_any = False
            while time.time() < deadline:
                try:
                    resp = await asyncio.wait_for(ws.recv(), timeout=1.0)
                    print(f"< {resp}")
                    got_any = True
                    # Most replies are single JSON objects; settle quickly.
                    deadline = min(deadline, time.time() + 0.5)
                except asyncio.TimeoutError:
                    if got_any:
                        break


async def interactive(host):
    uri = f"ws://{host}/ws"
    print(f"# Connecting to {uri}", file=sys.stderr)
    async with websockets.connect(uri, open_timeout=5) as ws:
        print(f"# Connected. Type commands (EOF to exit).", file=sys.stderr)
        async def reader():
            try:
                while True:
                    resp = await ws.recv()
                    print(f"< {resp}", flush=True)
            except websockets.ConnectionClosed:
                pass
        task = asyncio.create_task(reader())
        loop = asyncio.get_event_loop()
        try:
            while True:
                line = await loop.run_in_executor(None, sys.stdin.readline)
                if not line:
                    break
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    msg = parse_command_line(line)
                except Exception as e:
                    print(f"# parse error: {e}", file=sys.stderr)
                    continue
                print(f"> {json.dumps(msg)}", flush=True)
                await ws.send(json.dumps(msg))
        finally:
            task.cancel()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True, help="Dongle IP (e.g. 192.168.1.59 or 192.168.10.1)")
    ap.add_argument("--script", action="append", default=[],
                    help="One command line to send (repeatable). Order preserved.")
    ap.add_argument("--script-file", help="File with one command per line (# comments OK).")
    ap.add_argument("--interactive", action="store_true",
                    help="Read commands from stdin, stream responses to stdout.")
    ap.add_argument("--timeout", type=float, default=8.0,
                    help="Per-command response window in seconds (default 8).")
    args = ap.parse_args()

    if args.interactive:
        asyncio.run(interactive(args.host))
    else:
        cmds = list(args.script)
        if args.script_file:
            with open(args.script_file) as f:
                cmds.extend(f.readlines())
        if not cmds:
            ap.error("provide --script, --script-file, or --interactive")
        asyncio.run(drive(args.host, cmds, response_timeout=args.timeout))


if __name__ == "__main__":
    main()
