#!/usr/bin/env python3
"""mock_dongle.py — fixture-driven WebSocket + admin-HTTP shim for the
UI eval harness (Prompt 9).

Not a firmware substitute. The dongle's WS server returns command
responses driven by mock_dongle_responses.json; an admin HTTP port
fires scripted event sequences so the test harness can verify the
client's wsEvents subscriptions WITHOUT a real ESP32-S3 in the loop.

Architecture:
  - WS port (default 47821): clients connect and POST commands as
    JSON. The mock looks up the canned response by command name and
    sends it back, propagating the client's _cbId so the UI's
    callback machinery completes.
  - Admin port (default 47822): HTTP/1.x. Endpoints:
      POST /admin/fire     {sequence: NAME}
        Replays the named sequence from _sequences[]. Each step is
        {event, payload, delay_ms}; the mock waits the delay then
        broadcasts the JSON {event, ...payload} to every connected
        client.
      POST /admin/override {command: NAME, override_key: KEY|null}
        Subsequent responses to `command` use _overrides[KEY]
        instead of the canned default. Pass null to clear.
  - Run with --port WS_PORT --admin-port HTTP_PORT --fixture PATH.

This file is NOT a magic-number-bearing module — see UI_CONST in the
eval harness for the test runner's tunables. The defaults below
("Proposed default" annotated) carry the same Sean-approval pattern
as the firmware config headers.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
from pathlib import Path
from typing import Any, Optional, Set

try:
    import websockets
    from websockets.asyncio.server import ServerConnection, serve as ws_serve
except ImportError as e:
    print(f"mock_dongle.py: websockets package required: {e}", file=sys.stderr)
    print("Install: pip install --user websockets", file=sys.stderr)
    sys.exit(2)


# Defaults — every numeric named, "Proposed default — needs Sean's
# approval before lock." pattern.
DEFAULT_WS_PORT = 47821
DEFAULT_ADMIN_PORT = 47822
DEFAULT_HOST = "127.0.0.1"
ADMIN_BODY_LIMIT_BYTES = 65536  # cap admin POST bodies — Proposed default — needs Sean's approval before lock.
LOG_LEVEL_DEFAULT = "INFO"

# In-memory state.
_clients: Set[ServerConnection] = set()
_overrides: dict[str, str] = {}  # command_name -> override_key in fixture._overrides
_fixture: dict[str, Any] = {}

log = logging.getLogger("mock_dongle")


def load_fixture(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise SystemExit(f"fixture not found: {path}")
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise SystemExit("fixture must be a JSON object")
    return data


def lookup_response(command: str, params: Optional[dict[str, Any]]) -> dict[str, Any]:
    """Return the canned response for a command, applying any active
    test-set override. Missing commands receive a generic ok=false
    payload — that's the fixture's responsibility to populate, not
    the mock's to fabricate."""
    over_key = _overrides.get(command)
    if over_key is not None:
        overrides_block = _fixture.get("_overrides", {})
        if over_key in overrides_block:
            return dict(overrides_block[over_key])

    if command in _fixture and not command.startswith("_"):
        return dict(_fixture[command])

    return {
        "success": False,
        "ok": False,
        "command": command,
        "error": f"mock: command '{command}' has no fixture entry",
    }


async def _ws_handler(conn: ServerConnection) -> None:
    _clients.add(conn)
    log.info("WS connected: %s (clients=%d)", conn.remote_address, len(_clients))
    try:
        async for raw in conn:
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                await conn.send(json.dumps({"success": False, "error": "invalid JSON"}))
                continue
            if not isinstance(msg, dict):
                continue

            command = msg.get("command")
            cb_id = msg.get("_cbId")
            params = msg.get("params") if isinstance(msg.get("params"), dict) else None

            if not command:
                continue

            response = lookup_response(command, params)
            if cb_id is not None:
                response["_cbId"] = cb_id
            response.setdefault("command", command)

            await conn.send(json.dumps(response))
            log.debug("WS %s -> %s", command, response)
    except websockets.ConnectionClosed:
        pass
    finally:
        _clients.discard(conn)
        log.info("WS disconnected (clients=%d)", len(_clients))


async def _broadcast(payload: dict[str, Any]) -> None:
    if not _clients:
        return
    blob = json.dumps(payload)
    # Snapshot the set so a concurrent disconnect doesn't mutate during iter.
    targets = list(_clients)
    await asyncio.gather(*[c.send(blob) for c in targets], return_exceptions=True)


async def _fire_sequence(name: str) -> dict[str, Any]:
    sequences = _fixture.get("_sequences", {})
    seq = sequences.get(name)
    if not seq or not isinstance(seq, list):
        return {"ok": False, "error": f"unknown sequence: {name}"}

    fired = 0
    for step in seq:
        if not isinstance(step, dict):
            continue
        delay_ms = int(step.get("delay_ms", 0))
        if delay_ms > 0:
            await asyncio.sleep(delay_ms / 1000.0)
        event_name = step.get("event")
        if not event_name:
            continue
        payload = {"event": event_name}
        if isinstance(step.get("payload"), dict):
            payload.update(step["payload"])
        await _broadcast(payload)
        fired += 1
        log.debug("seq %s fired %s", name, event_name)
    return {"ok": True, "sequence": name, "fired": fired}


async def _admin_handle_request(method: str, path: str, body: bytes) -> tuple[int, bytes]:
    if method != "POST":
        return 405, b'{"error":"method not allowed"}'
    try:
        payload = json.loads(body.decode("utf-8")) if body else {}
    except (json.JSONDecodeError, UnicodeDecodeError):
        return 400, b'{"error":"invalid JSON body"}'

    if path == "/admin/fire":
        seq = payload.get("sequence")
        if not isinstance(seq, str):
            return 400, b'{"error":"missing sequence"}'
        result = await _fire_sequence(seq)
        return 200, json.dumps(result).encode("utf-8")

    if path == "/admin/override":
        cmd = payload.get("command")
        key = payload.get("override_key")
        if not isinstance(cmd, str):
            return 400, b'{"error":"missing command"}'
        if key is None:
            _overrides.pop(cmd, None)
        else:
            _overrides[cmd] = str(key)
        return 200, json.dumps({"ok": True, "command": cmd, "override_key": key}).encode("utf-8")

    return 404, b'{"error":"not found"}'


async def _admin_serve(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        # Parse minimal HTTP/1.1.
        request_line = await reader.readline()
        if not request_line:
            return
        try:
            method, path, _ = request_line.decode("ascii").rstrip().split(" ", 2)
        except ValueError:
            writer.write(b"HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n")
            await writer.drain()
            return

        content_length = 0
        while True:
            line = await reader.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            try:
                k, v = line.decode("ascii").rstrip().split(":", 1)
                if k.lower().strip() == "content-length":
                    content_length = int(v.strip())
            except ValueError:
                continue

        if content_length > ADMIN_BODY_LIMIT_BYTES:
            writer.write(b"HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n\r\n")
            await writer.drain()
            return

        body = b""
        if content_length > 0:
            body = await reader.readexactly(content_length)

        status, resp_body = await _admin_handle_request(method, path, body)
        writer.write(
            f"HTTP/1.1 {status} OK\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {len(resp_body)}\r\n"
            f"Connection: close\r\n"
            f"\r\n".encode("ascii")
        )
        writer.write(resp_body)
        await writer.drain()
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass


async def main_async(args: argparse.Namespace) -> int:
    global _fixture
    _fixture = load_fixture(Path(args.fixture).resolve())
    log.info("loaded fixture: %s (top-level keys=%d)", args.fixture, len(_fixture))

    ws_server = await ws_serve(_ws_handler, args.host, args.port)
    admin_server = await asyncio.start_server(_admin_serve, args.host, args.admin_port)
    log.info("WS  listening on ws://%s:%d/", args.host, args.port)
    log.info("ADMIN listening on http://%s:%d/admin/{fire,override}", args.host, args.admin_port)

    try:
        async with admin_server:
            # Run until cancelled (eval kills the subprocess).
            stop_evt = asyncio.Event()
            try:
                await stop_evt.wait()
            except asyncio.CancelledError:
                pass
    finally:
        ws_server.close()
        await ws_server.wait_closed()
    return 0


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description="FUTUNER mock dongle (UI eval harness)")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=DEFAULT_WS_PORT, help="WS port")
    p.add_argument("--admin-port", type=int, default=DEFAULT_ADMIN_PORT, help="Admin HTTP port")
    p.add_argument(
        "--fixture",
        default=str(Path(__file__).resolve().parent / "mock_dongle_responses.json"),
        help="Fixture JSON path",
    )
    p.add_argument("--log-level", default=LOG_LEVEL_DEFAULT)
    args = p.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )

    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
