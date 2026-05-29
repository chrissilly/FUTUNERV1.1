"""
sillyrabbitmotorsport.com — SRM Cloud server
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

# P-67: WOT telemetry-log ingest storage. The dongle uploads gzipped
# CSV WOT logs; we store one file per upload under WOT_LOG_DIR/<mac>/.
# Kept ROOT-relative (like FW_DIR / CAL_DIR) so it lives inside the
# container's mounted data volume — a literal host path like
# /cal/wot_logs would not exist inside the FastAPI container. Override
# with the WOT_LOG_DIR env var if the deployment wants an external
# mount. (The dongle's own /cal/wot path is unrelated — that's the
# dongle's flash partition.)
WOT_LOG_DIR        = Path(os.environ.get("WOT_LOG_DIR", str(ROOT / "wot_logs")))
WOT_LOG_MAX_BYTES  = 64 * 1024          # 64 KB cap per WOT spec (logs are ~3-4 KB)
GZIP_MAGIC         = b"\x1f\x8b"        # RFC-1952 gzip stream magic

# AUDIT C10: single-file SQLite serializes writes. PHASE 1 adds the first
# write-heavy customer tables (sessions on every login, license churn), so
# a connection that hits a held write-lock should wait, not fail instantly
# with "database is locked". busy_timeout makes SQLite block up to N ms for
# the lock. Env-overridable per Rule 3 (no magic literals).
DB_BUSY_TIMEOUT_MS = int(os.environ.get("DB_BUSY_TIMEOUT_MS", "5000"))

DATA_DIR.mkdir(exist_ok=True)
FW_DIR.mkdir(exist_ok=True)
CAL_DIR.mkdir(exist_ok=True)
WOT_LOG_DIR.mkdir(exist_ok=True)

# ===========================================================================
# Database (SQLite — one file, no service to run)
# ===========================================================================
def db():
    """Get a sqlite connection. One per request is fine for our scale."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute(f"PRAGMA busy_timeout = {DB_BUSY_TIMEOUT_MS}")
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

    -- P-67: WOT telemetry log uploads. One row per uploaded gzip CSV.
    -- file_path is relative to WOT_LOG_DIR. mac/vin copied from the
    -- authenticating device row at upload time (NOT client-claimed).
    CREATE TABLE IF NOT EXISTS wot_logs (
        id               INTEGER PRIMARY KEY AUTOINCREMENT,
        mac              TEXT NOT NULL,
        vin              TEXT,
        uploaded_at      INTEGER NOT NULL,
        file_path        TEXT NOT NULL,
        byte_count       INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_wot_logs_mac_ts
        ON wot_logs (mac, uploaded_at DESC);

    -- ===================================================================
    -- PHASE 1 customer model (users / sessions / vehicles / licenses /
    -- stripe_events). Kept in LOCKSTEP with
    -- migrations/20260529_user_vehicle_license.sql — that file is the
    -- auditable record + already-live-DB apply path; this block is the
    -- fresh-deploy path (Dockerfile copies only src/, so the container
    -- never sees migrations/). Edit BOTH or neither. New tables use
    -- ISO-8601 UTC TEXT timestamps (legacy device tables use INTEGER
    -- unix; no cross-type comparison occurs between them).
    -- ===================================================================
    CREATE TABLE IF NOT EXISTS users (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        email         TEXT    NOT NULL UNIQUE,
        password_hash TEXT    NOT NULL,
        verified      INTEGER NOT NULL DEFAULT 0,
        created_at    TEXT    NOT NULL
    );

    CREATE TABLE IF NOT EXISTS sessions (
        token      TEXT    PRIMARY KEY,
        user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        expires_at TEXT    NOT NULL,
        created_at TEXT    NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_sessions_user    ON sessions(user_id);
    CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);

    CREATE TABLE IF NOT EXISTS vehicles (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        user_id     INTEGER NOT NULL REFERENCES users(id)    ON DELETE CASCADE,
        vin         TEXT    NOT NULL,
        dongle_mac  TEXT    NOT NULL REFERENCES devices(mac) ON DELETE RESTRICT,
        ecu_boxcode TEXT,
        nickname    TEXT,
        created_at  TEXT    NOT NULL,
        deleted_at  TEXT
    );
    CREATE UNIQUE INDEX IF NOT EXISTS idx_vehicles_vin_active
        ON vehicles(vin) WHERE deleted_at IS NULL;
    CREATE INDEX IF NOT EXISTS idx_vehicles_user ON vehicles(user_id);
    CREATE INDEX IF NOT EXISTS idx_vehicles_mac  ON vehicles(dongle_mac);

    CREATE TABLE IF NOT EXISTS licenses (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        vehicle_id  INTEGER NOT NULL REFERENCES vehicles(id) ON DELETE RESTRICT,
        tier        TEXT    NOT NULL,
        state       TEXT    NOT NULL,
        valid_from  TEXT,
        valid_until TEXT,
        token       TEXT    UNIQUE,
        created_at  TEXT    NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_licenses_vehicle     ON licenses(vehicle_id);
    CREATE INDEX IF NOT EXISTS idx_licenses_state_until ON licenses(state, valid_until);

    CREATE TABLE IF NOT EXISTS stripe_events (
        id           INTEGER PRIMARY KEY AUTOINCREMENT,
        event_id     TEXT    UNIQUE,
        payload_json TEXT    NOT NULL,
        received_at  TEXT    NOT NULL,
        processed_at TEXT
    );
    """)
    # Idempotent license-column migrations. SCALE_ARCHITECTURE §6 adds
    # paid / revoked / revoked_reason to the devices table. SQLite's
    # ALTER TABLE ADD COLUMN is the simplest forward path; raises
    # OperationalError if the column already exists, which we catch.
    # Real Alembic migrations land with the PostgreSQL move per
    # SCALE_ARCHITECTURE Phase A.
    for col_ddl in (
        "paid INTEGER DEFAULT 0",
        "revoked INTEGER DEFAULT 0",
        "revoked_reason TEXT",
    ):
        try:
            conn.execute(f"ALTER TABLE devices ADD COLUMN {col_ddl}")
        except sqlite3.OperationalError:
            pass
    conn.commit()
    conn.close()


def normalize_vin(v: Optional[str]) -> Optional[str]:
    """ISO-3779 normalization: trim whitespace, uppercase. Returns
    None if input is None or normalizes to empty. Applied at the
    comparison site only — the raw value the ECU emitted is preserved
    in the DB for audit."""
    if v is None:
        return None
    s = v.strip().upper()
    return s if s else None

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

def device_for_x_device_auth(x_device_auth: Optional[str]) -> sqlite3.Row:
    """P-67: resolve a device row from the X-Device-Auth header.

    Same lookup as device_for_token (token → devices.auth_token row)
    but for the bare-token X-Device-Auth header the dongle's telemetry
    uploader sends (no 'Bearer ' prefix). The MAC/VIN we trust come
    from THIS row — never from anything the client puts in the body.
    """
    if not x_device_auth or not x_device_auth.strip():
        raise HTTPException(401, "Missing X-Device-Auth header")
    token = x_device_auth.strip()
    conn = db()
    row = conn.execute(
        "SELECT * FROM devices WHERE auth_token = ?", (token,)
    ).fetchone()
    conn.close()
    if not row:
        raise HTTPException(401, "Unknown device token")
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

    VIN-mismatch behavior: if device record already has a vin set
    AND incoming vin (after ISO-3779 normalize) differs, return 409
    Conflict. Cases:
      - request.vin None/empty                 → no VIN check, no 409
      - device.vin   None/empty (first-pair)   → set it, no 409
      - both set, normalized equal             → idempotent, no 409
      - both set, normalized differ            → 409
    Comparison is normalized; storage preserves raw ECU bytes.
    """
    dev = device_for_token(authorization)
    incoming_vin = normalize_vin(payload.vin)
    existing_vin = normalize_vin(dev["vin"])
    if incoming_vin and existing_vin and incoming_vin != existing_vin:
        log(dev["mac"], "register_conflict",
            f"existing={existing_vin} incoming={incoming_vin}")
        raise HTTPException(
            409,
            f"VIN already paired (existing={existing_vin}, incoming={incoming_vin})"
        )
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


@app.get("/api/v1/license")
async def get_license(authorization: Optional[str] = Header(None)):
    """
    Per-device license state. Returns:
      { paid: bool, vin: str, revoked: bool, revoked_reason: str|null }

    `paid` and `revoked` are stored as INTEGER 0/1 on disk and
    surfaced as bool. `vin` is the raw value the ECU originally
    reported (NOT normalized — the dongle does its own normalization
    at the comparison site, and audit tools want the raw bytes).
    """
    dev = device_for_token(authorization)
    return {
        "paid": bool((dev["paid"] if "paid" in dev.keys() else 0) or 0),
        "vin": dev["vin"] or "",
        "revoked": bool((dev["revoked"] if "revoked" in dev.keys() else 0) or 0),
        "revoked_reason": (dev["revoked_reason"] if "revoked_reason" in dev.keys() else None),
    }


@app.post("/api/v1/telemetry/log")
async def telemetry_log(
    request: Request,
    x_device_auth: Optional[str] = Header(None),
):
    """
    P-67: WOT telemetry log ingest. The dongle POSTs a gzip-compressed
    CSV WOT log here after a wide-open-throttle pull.

    Auth:    X-Device-Auth: <auth_token>  (bare token, no Bearer prefix).
             MAC + VIN are taken from the resolved device row — the
             request body is never trusted to claim its own identity.
    Body:    raw gzip bytes (Content-Type application/gzip; we also
             verify the 1F 8B gzip magic regardless of Content-Type).
    Gate:    device must be paid (and not revoked) — free tier doesn't
             upload telemetry.
    Limits:  body must be 1..WOT_LOG_MAX_BYTES (64 KB).

    Returns 200 {ok:true, log_id:N} on success; 401 bad/missing auth;
    403 unpaid/revoked; 400 empty or non-gzip body; 413 oversized.

    Storage is atomic from the client's view: a 200 is returned only
    if BOTH the file write and the DB row insert succeeded. On any
    failure after the file is written, the file is removed so no
    orphan is left without a row.
    """
    dev = device_for_x_device_auth(x_device_auth)

    paid    = bool((dev["paid"]    if "paid"    in dev.keys() else 0) or 0)
    revoked = bool((dev["revoked"] if "revoked" in dev.keys() else 0) or 0)
    if not paid or revoked:
        raise HTTPException(403, "telemetry upload requires an active (non-revoked) license")

    body = await request.body()
    if not body:
        raise HTTPException(400, "empty body")
    if len(body) > WOT_LOG_MAX_BYTES:
        raise HTTPException(413, f"log exceeds {WOT_LOG_MAX_BYTES} byte cap")
    if body[:2] != GZIP_MAGIC:
        raise HTTPException(400, "body is not a gzip stream (missing 1F 8B magic)")

    mac = dev["mac"]
    vin = dev["vin"]
    now = int(time.time())

    # Per-device subdir; unique server-timestamped filename (short hex
    # suffix avoids same-second collisions on one device).
    dev_dir = WOT_LOG_DIR / mac.replace(":", "")
    dev_dir.mkdir(parents=True, exist_ok=True)
    fname = f"{now}_{secrets.token_hex(4)}.csv.gz"
    fpath = dev_dir / fname
    rel_path = str(fpath.relative_to(WOT_LOG_DIR))

    # Write file first, then insert the row. If the insert fails,
    # unlink the file so we never leave a file without a tracking row.
    try:
        fpath.write_bytes(body)
    except OSError as e:
        raise HTTPException(500, f"storage write failed: {e}")

    conn = db()
    try:
        cur = conn.execute(
            "INSERT INTO wot_logs (mac, vin, uploaded_at, file_path, byte_count) "
            "VALUES (?, ?, ?, ?, ?)",
            (mac, vin, now, rel_path, len(body))
        )
        log_id = cur.lastrowid
        conn.commit()
    except Exception as e:
        conn.close()
        # roll back the file so file + row stay consistent
        try:
            fpath.unlink()
        except OSError:
            pass
        raise HTTPException(500, f"storage index failed: {e}")
    conn.close()

    log(mac, "wot_upload", f"log_id={log_id} bytes={len(body)} path={rel_path}")
    return {"ok": True, "log_id": log_id}


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


@app.get("/admin/devices/{mac}")
async def admin_get_device(mac: str, x_admin_key: Optional[str] = Header(None)):
    """
    P-43: single-row selector for an enrolled device. Mirrors the
    admin_set_license auth model (require_admin) and 404 shape
    ("No such device: <mac>") used by POST /admin/devices/{mac}/license.
    Returns the same per-row shape that admin_list_devices returns —
    so tools that want to confirm a specific dongle's enrollment +
    paid state don't have to round-trip the full list.
    """
    require_admin(x_admin_key)
    conn = db()
    row = conn.execute("SELECT * FROM devices WHERE mac = ?", (mac,)).fetchone()
    conn.close()
    if not row:
        raise HTTPException(404, f"No such device: {mac}")
    return dict(row)


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


class LicenseSetPayload(BaseModel):
    paid:           Optional[int] = None  # 0 or 1
    revoked:        Optional[int] = None  # 0 or 1
    revoked_reason: Optional[str] = None  # set to None to clear

@app.post("/admin/devices/{mac}/license")
async def admin_set_license(
    mac: str,
    payload: LicenseSetPayload,
    x_admin_key: Optional[str] = Header(None),
):
    """
    Admin: flip the license state on a device. Used after a one-time
    payment lands (paid=1) or for fraud/safety revocation
    (revoked=1, revoked_reason='...').
    """
    require_admin(x_admin_key)
    conn = db()
    row = conn.execute("SELECT 1 FROM devices WHERE mac = ?", (mac,)).fetchone()
    if not row:
        conn.close(); raise HTTPException(404, f"No such device: {mac}")
    sets, params = [], []
    if payload.paid is not None:
        sets.append("paid = ?"); params.append(1 if payload.paid else 0)
    if payload.revoked is not None:
        sets.append("revoked = ?"); params.append(1 if payload.revoked else 0)
    if payload.revoked_reason is not None or payload.revoked == 0:
        sets.append("revoked_reason = ?")
        params.append(payload.revoked_reason or None)
    if not sets:
        conn.close(); raise HTTPException(400, "No license fields supplied")
    params.append(mac)
    conn.execute(f"UPDATE devices SET {', '.join(sets)} WHERE mac = ?", params)
    conn.commit()
    conn.close()
    log(mac, "license_set", payload.model_dump_json())
    return {"ok": True, "mac": mac}


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
            "POST /api/v1/telemetry/log",
            "GET  /api/v1/device/update_available",
            "GET  /api/v1/device/download_update/{filename}",
            "GET  /api/v1/device/calibration",
            "POST /admin/devices",
            "PUT  /admin/firmware/{build_hash}",
            "POST /admin/devices/{mac}/assign_firmware",
            "POST /admin/calibrations/{filename}",
            "POST /admin/devices/{mac}/assign_calibration",
            "GET  /admin/devices",
            "GET  /admin/devices/{mac}",
            "GET  /admin/log/{mac}",
        ],
    }

@app.get("/health")
async def health():
    return {"ok": True, "ts": int(time.time())}
