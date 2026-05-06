"""
api.sillyrabbitmotorsport.com — SRM Cloud server
================================================

Replaces api.dynoscorpion.com (Sean's server) with one Chris/SRM owns.
Implements the same protocol the v1.0 SEFI dongle expects:

    POST /api/v1/device/register
    GET  /api/v1/device/update_available
    GET  /api/v1/device/download_update/{filename}

Plus auth-gated calibration delivery (the per-VIN SBF patcher) and a
small admin surface for managing devices and firmware.

Tech: FastAPI + SQLite. Single file, ~400 lines, easy to read and
extend. Run with `uvicorn src.main:app --host 0.0.0.0 --port 8000`.
Front it with Caddy or nginx for TLS (see Caddyfile in this folder).

Auth model
----------
Every dongle has a unique Bearer token (stored in NVS at key
`auth_token`). The dongle includes it as `Authorization: Bearer <token>`
on every request. The server looks the token up in the `devices` table.
- Token unknown      → 401
- Token known        → grants access scoped to the device that owns it

Admin endpoints (for you, not the dongles) use a separate
ADMIN_API_KEY env var that you'll set during deployment.

Initial setup
-------------
1. Set ADMIN_API_KEY env var to a long random string.
2. POST /admin/devices to enroll your dongle (returns a Bearer token).
3. PUT /admin/firmware/<hash> with the firmware binary to register a build.
4. POST /admin/devices/<mac>/assign_firmware to push a build to a device.
5. POST /admin/calibrations/<filename> with an SBF binary to host a tune.
6. POST /admin/devices/<mac>/assign_calibration to assign a tune.

The dongle picks up everything on its next boot/poll cycle.
"""
from __future__ import annotations
import os, json, sqlite3, secrets, time, hashlib, struct, hmac
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException, Header, Request, UploadFile, File
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from pydantic import BaseModel, Field

# ===========================================================================
# Config
# ===========================================================================
ROOT          = Path(__file__).resolve().parent.parent
DATA_DIR      = ROOT / "data"
FW_DIR        = ROOT / "firmware"
CAL_DIR       = ROOT / "calibrations"
DB_PATH       = DATA_DIR / "srm.db"
ADMIN_API_KEY = os.environ.get("ADMIN_API_KEY", "")  # MUST be set in production

DATA_DIR.mkdir(exist_ok=True)
FW_DIR.mkdir(exist_ok=True)
CAL_DIR.mkdir(exist_ok=True)

# ===========================================================================
# Database (SQLite — one file, no service to run)
# ===========================================================================
def db():
    """Get a sqlite connection. One per request is fine for our scale."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn

def init_db():
    conn = db()
    conn.executescript("""
    CREATE TABLE IF NOT EXISTS devices (
        mac              TEXT PRIMARY KEY,
        auth_token       TEXT NOT NULL UNIQUE,
        vin              TEXT,
        boxcode          TEXT,
        owner_name       TEXT,
        owner_email      TEXT,
        dealer_id        TEXT,
        active_firmware  TEXT,        -- references firmware.build_hash
        active_cal       TEXT,        -- references calibrations.filename
        last_seen        INTEGER,     -- unix ts
        created_at       INTEGER,
        ethanol_random   INTEGER      -- the per-ECU validation value (0..1023 for 10-bit)
    );

    CREATE TABLE IF NOT EXISTS firmware (
        build_hash       TEXT PRIMARY KEY,
        filename         TEXT NOT NULL,
        build_number     INTEGER,
        size_bytes       INTEGER,
        sha256           TEXT,
        notes            TEXT,
        created_at       INTEGER,
        is_release       INTEGER DEFAULT 0
    );

    CREATE TABLE IF NOT EXISTS calibrations (
        filename         TEXT PRIMARY KEY,
        boxcode          TEXT NOT NULL,
        version          INTEGER,
        size_bytes       INTEGER,
        sha256           TEXT,        -- of the unpatched base file
        ethanol_bit_count INTEGER,
        notes            TEXT,
        created_at       INTEGER
    );

    CREATE TABLE IF NOT EXISTS device_log (
        id               INTEGER PRIMARY KEY AUTOINCREMENT,
        mac              TEXT,
        ts               INTEGER,
        event            TEXT,
        detail           TEXT
    );
    """)
    conn.commit()
    conn.close()

# ===========================================================================
# Auth helpers
# ===========================================================================
def device_for_token(authorization: Optional[str]) -> sqlite3.Row:
    if not authorization or not authorization.lower().startswith("bearer "):
        raise HTTPException(401, "Missing Bearer token")
    token = authorization[7:].strip()
    if not token:
        raise HTTPException(401, "Empty Bearer token")
    conn = db()
    row = conn.execute(
        "SELECT * FROM devices WHERE auth_token = ?", (token,)
    ).fetchone()
    conn.close()
    if not row:
        raise HTTPException(401, "Unknown token")
    return row

def require_admin(api_key: Optional[str]):
    if not ADMIN_API_KEY:
        raise HTTPException(500, "Server: ADMIN_API_KEY not configured")
    if not api_key or not hmac.compare_digest(api_key, ADMIN_API_KEY):
        raise HTTPException(403, "Bad admin key")

def log(mac: str, event: str, detail: str = ""):
    conn = db()
    conn.execute(
        "INSERT INTO device_log (mac, ts, event, detail) VALUES (?, ?, ?, ?)",
        (mac, int(time.time()), event, detail[:500])
    )
    conn.commit()
    conn.close()

# ===========================================================================
# SBF patcher — injects per-ECU ethanol_random into base SBF before delivery
# ===========================================================================
SCPN_HEADER_OFFSET_ETHANOL_BIT_COUNT = 4 + 4*9   # 40
SCPN_HEADER_OFFSET_ETHANOL_RANDOM    = 4 + 4*10  # 44
# Header layout: 'SCPN' magic + 11 u32 LE — see communication_v1_uds_albin.md

def patch_sbf_for_device(sbf_bytes: bytes, ethanol_random: int) -> bytes:
    """
    Take a base SBF and inject this device's ethanol_random validation
    value at the right header offset. The dongle then refuses any other
    SBF whose value doesn't match this device's ECU register.

    Returns the patched bytes (same length as input).
    """
    if not sbf_bytes.startswith(b"SCPN"):
        raise HTTPException(400, "Not an SCPN/SBF file")
    if len(sbf_bytes) < SCPN_HEADER_OFFSET_ETHANOL_RANDOM + 4:
        raise HTTPException(400, "SBF too short for header")
    out = bytearray(sbf_bytes)
    out[SCPN_HEADER_OFFSET_ETHANOL_RANDOM:SCPN_HEADER_OFFSET_ETHANOL_RANDOM+4] = \
        struct.pack("<I", ethanol_random & 0xFFFFFFFF)
    return bytes(out)

# ===========================================================================
# FastAPI app
# ===========================================================================
@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    yield

app = FastAPI(
    title="SRM Cloud",
    version="0.1.0",
    description="Silly Rabbit Motorsport device-management cloud — replaces api.dynoscorpion.com",
    lifespan=lifespan,
)

# ---------------------------------------------------------------------------
# Device-facing endpoints (the three the dongle calls)
# ---------------------------------------------------------------------------

class RegisterPayload(BaseModel):
    mac:        Optional[str] = None
    vin:        Optional[str] = None
    boxcode:    Optional[str] = None
    build_hash: Optional[str] = None
    active_cal: Optional[str] = None
    fw_version: Optional[str] = None

@app.post("/api/v1/device/register")
async def device_register(
    payload: RegisterPayload,
    authorization: Optional[str] = Header(None),
):
    """
    Dongle phones home on every boot.
    Body: { mac, vin, boxcode, build_hash, active_cal, fw_version }
    Returns: { ok: true, server_time, message? }
    """
    dev = device_for_token(authorization)
    conn = db()
    conn.execute(
        """UPDATE devices SET
              vin             = COALESCE(?, vin),
              boxcode         = COALESCE(?, boxcode),
              last_seen       = ?
            WHERE mac = ?""",
        (payload.vin, payload.boxcode, int(time.time()), dev["mac"])
    )
    conn.commit()
    conn.close()
    log(dev["mac"], "register", payload.model_dump_json())
    return {
        "ok": True,
        "server_time": int(time.time()),
        "message": "Welcome to SRM",
    }


@app.get("/api/v1/device/update_available")
async def update_available(
    authorization: Optional[str] = Header(None),
    current_hash: Optional[str] = None,
):
    """
    Dongle asks: "is there a newer firmware than what I'm running?"
    Returns: { available: 0|1, build_hash, build_number, url }
    """
    dev = device_for_token(authorization)
    target = dev["active_firmware"]
    if not target:
        # If admin hasn't assigned a firmware to this device, fall back to
        # the latest is_release=1 build.
        conn = db()
        row = conn.execute(
            "SELECT build_hash FROM firmware WHERE is_release = 1 "
            "ORDER BY created_at DESC LIMIT 1"
        ).fetchone()
        conn.close()
        target = row["build_hash"] if row else None

    if not target or target == current_hash:
        return {"available": 0}

    conn = db()
    fw = conn.execute(
        "SELECT * FROM firmware WHERE build_hash = ?", (target,)
    ).fetchone()
    conn.close()
    if not fw:
        return {"available": 0}

    return {
        "available": 1,
        "build_hash":   fw["build_hash"],
        "build_number": fw["build_number"],
        "url":          f"/api/v1/device/download_update/{fw['filename']}",
        "size_bytes":   fw["size_bytes"],
        "sha256":       fw["sha256"],
    }


@app.get("/api/v1/device/download_update/{filename}")
async def download_update(
    filename: str,
    authorization: Optional[str] = Header(None),
):
    """
    Dongle downloads the firmware binary. Token-gated.
    """
    dev = device_for_token(authorization)
    # Reject path traversal
    if "/" in filename or ".." in filename:
        raise HTTPException(400, "Bad filename")
    fw_path = FW_DIR / filename
    if not fw_path.is_file():
        raise HTTPException(404, "Firmware not found")
    log(dev["mac"], "download_update", filename)
    return FileResponse(
        fw_path,
        media_type="application/octet-stream",
        filename=filename,
    )


@app.get("/api/v1/device/calibration")
async def get_assigned_calibration(
    authorization: Optional[str] = Header(None),
):
    """
    Dongle pulls its currently-assigned SBF, patched with this device's
    ethanol_random validation value. This is the per-VIN DRM Sean had.
    """
    dev = device_for_token(authorization)
    if not dev["active_cal"]:
        raise HTTPException(404, "No calibration assigned")
    cal_path = CAL_DIR / dev["active_cal"]
    if not cal_path.is_file():
        raise HTTPException(500, "Assigned cal file missing on disk")

    base = cal_path.read_bytes()
    if dev["ethanol_random"] is not None:
        patched = patch_sbf_for_device(base, int(dev["ethanol_random"]))
    else:
        patched = base   # device hasn't been ethanol-pinned yet

    log(dev["mac"], "fetch_calibration", dev["active_cal"])
    return StreamingResponse(
        iter([patched]),
        media_type="application/octet-stream",
        headers={"Content-Disposition": f"attachment; filename={dev['active_cal']}"},
    )


# ---------------------------------------------------------------------------
# Admin surface — for you to manage devices/firmware/cals
# ---------------------------------------------------------------------------

class EnrollPayload(BaseModel):
    mac:           str
    vin:           Optional[str] = None
    boxcode:       Optional[str] = None
    owner_name:    Optional[str] = None
    owner_email:   Optional[str] = None
    dealer_id:     Optional[str] = None
    ethanol_random: Optional[int] = None  # the per-ECU 10-bit validation value

@app.post("/admin/devices")
async def admin_enroll_device(
    payload: EnrollPayload,
    x_admin_key: Optional[str] = Header(None),
):
    """
    Enroll a new dongle. Returns the Bearer token to install in NVS.
    """
    require_admin(x_admin_key)
    token = secrets.token_hex(16)  # 32 char hex — same width as Sean's tokens
    conn = db()
    try:
        conn.execute(
            """INSERT INTO devices
               (mac, auth_token, vin, boxcode, owner_name, owner_email,
                dealer_id, ethanol_random, last_seen, created_at)
               VALUES (?,?,?,?,?,?,?,?,?,?)""",
            (payload.mac, token, payload.vin, payload.boxcode,
             payload.owner_name, payload.owner_email, payload.dealer_id,
             payload.ethanol_random, 0, int(time.time()))
        )
        conn.commit()
    except sqlite3.IntegrityError:
        conn.close()
        raise HTTPException(409, f"Device {payload.mac} already enrolled")
    conn.close()
    return {"ok": True, "mac": payload.mac, "auth_token": token}


@app.get("/admin/devices")
async def admin_list_devices(x_admin_key: Optional[str] = Header(None)):
    require_admin(x_admin_key)
    conn = db()
    rows = conn.execute("SELECT * FROM devices ORDER BY created_at DESC").fetchall()
    conn.close()
    return [dict(r) for r in rows]


@app.put("/admin/firmware/{build_hash}")
async def admin_upload_firmware(
    build_hash: str,
    file: UploadFile = File(...),
    build_number: Optional[int] = None,
    notes: Optional[str] = None,
    is_release: int = 0,
    x_admin_key: Optional[str] = Header(None),
):
    """
    Upload a new firmware build. Use the git short-hash as build_hash
    (e.g. b62fa86) — that matches the format the dongle reports back.
    """
    require_admin(x_admin_key)
    fname = file.filename or f"{build_hash}.bin"
    if "/" in fname or ".." in fname:
        raise HTTPException(400, "Bad filename")
    body = await file.read()
    sha = hashlib.sha256(body).hexdigest()
    out = FW_DIR / fname
    out.write_bytes(body)
    conn = db()
    conn.execute(
        """INSERT OR REPLACE INTO firmware
           (build_hash, filename, build_number, size_bytes, sha256, notes,
            created_at, is_release)
           VALUES (?,?,?,?,?,?,?,?)""",
        (build_hash, fname, build_number, len(body), sha, notes,
         int(time.time()), is_release)
    )
    conn.commit()
    conn.close()
    return {"ok": True, "build_hash": build_hash, "filename": fname,
            "size_bytes": len(body), "sha256": sha}


@app.post("/admin/devices/{mac}/assign_firmware")
async def admin_assign_firmware(
    mac: str,
    build_hash: str,
    x_admin_key: Optional[str] = Header(None),
):
    require_admin(x_admin_key)
    conn = db()
    fw = conn.execute(
        "SELECT 1 FROM firmware WHERE build_hash = ?", (build_hash,)
    ).fetchone()
    if not fw:
        conn.close(); raise HTTPException(404, f"No such build_hash: {build_hash}")
    conn.execute("UPDATE devices SET active_firmware = ? WHERE mac = ?",
                 (build_hash, mac))
    conn.commit()
    conn.close()
    return {"ok": True, "mac": mac, "active_firmware": build_hash}


class CalUploadResp(BaseModel):
    ok: bool
    filename: str
    size_bytes: int
    sha256: str

@app.post("/admin/calibrations/{filename}", response_model=CalUploadResp)
async def admin_upload_calibration(
    filename: str,
    file: UploadFile = File(...),
    boxcode: str = "",
    notes: Optional[str] = None,
    x_admin_key: Optional[str] = Header(None),
):
    require_admin(x_admin_key)
    if "/" in filename or ".." in filename:
        raise HTTPException(400, "Bad filename")
    body = await file.read()
    if not body.startswith(b"SCPN"):
        raise HTTPException(400, "Not an SCPN/SBF — magic byte mismatch")
    sha = hashlib.sha256(body).hexdigest()
    out = CAL_DIR / filename
    out.write_bytes(body)
    # Parse header so we know what we accepted
    bit_count = struct.unpack_from("<I", body, SCPN_HEADER_OFFSET_ETHANOL_BIT_COUNT)[0]
    version   = struct.unpack_from("<I", body, 4)[0]
    conn = db()
    conn.execute(
        """INSERT OR REPLACE INTO calibrations
           (filename, boxcode, version, size_bytes, sha256, ethanol_bit_count,
            notes, created_at)
           VALUES (?,?,?,?,?,?,?,?)""",
        (filename, boxcode, version, len(body), sha, bit_count, notes,
         int(time.time()))
    )
    conn.commit()
    conn.close()
    return {"ok": True, "filename": filename, "size_bytes": len(body), "sha256": sha}


@app.post("/admin/devices/{mac}/assign_calibration")
async def admin_assign_calibration(
    mac: str, filename: str,
    x_admin_key: Optional[str] = Header(None),
):
    require_admin(x_admin_key)
    conn = db()
    if not conn.execute(
        "SELECT 1 FROM calibrations WHERE filename = ?", (filename,)
    ).fetchone():
        conn.close(); raise HTTPException(404, f"No such cal: {filename}")
    conn.execute("UPDATE devices SET active_cal = ? WHERE mac = ?",
                 (filename, mac))
    conn.commit()
    conn.close()
    return {"ok": True, "mac": mac, "active_cal": filename}


@app.get("/admin/log/{mac}")
async def admin_device_log(
    mac: str,
    limit: int = 100,
    x_admin_key: Optional[str] = Header(None),
):
    require_admin(x_admin_key)
    conn = db()
    rows = conn.execute(
        "SELECT ts, event, detail FROM device_log WHERE mac = ? "
        "ORDER BY ts DESC LIMIT ?", (mac, limit)
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


# ---------------------------------------------------------------------------
# Health / version (open)
# ---------------------------------------------------------------------------
@app.get("/")
async def root():
    return {
        "service":  "SRM Cloud",
        "version":  "0.1.0",
        "endpoints": [
            "POST /api/v1/device/register",
            "GET  /api/v1/device/update_available",
            "GET  /api/v1/device/download_update/{filename}",
            "GET  /api/v1/device/calibration",
            "POST /admin/devices",
            "PUT  /admin/firmware/{build_hash}",
            "POST /admin/devices/{mac}/assign_firmware",
            "POST /admin/calibrations/{filename}",
            "POST /admin/devices/{mac}/assign_calibration",
            "GET  /admin/devices",
            "GET  /admin/log/{mac}",
        ],
    }

@app.get("/health")
async def health():
    return {"ok": True, "ts": int(time.time())}
