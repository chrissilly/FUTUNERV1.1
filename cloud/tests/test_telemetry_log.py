"""
P-67: tests for POST /api/v1/telemetry/log (WOT telemetry ingest).

Run with: PYTHONPATH=. pytest tests/test_telemetry_log.py

Mirrors test_smoke.py's setup: point the app at a fresh tempdir
BEFORE importing, set a test admin key, init the DB, drive the
endpoint via FastAPI TestClient. Exercises: gzip upload happy path,
auth pass/fail, the paid gate, the 413 oversized cap, and the
non-gzip-body rejection.
"""
import os, sys, gzip, io, tempfile
from pathlib import Path
import pytest

TMP = Path(tempfile.mkdtemp(prefix="srm-tlog-"))
(TMP / "data").mkdir()
(TMP / "firmware").mkdir()
(TMP / "calibrations").mkdir()
(TMP / "wot_logs").mkdir()

import src.main as m
m.DATA_DIR    = TMP / "data"
m.FW_DIR      = TMP / "firmware"
m.CAL_DIR     = TMP / "calibrations"
m.WOT_LOG_DIR = TMP / "wot_logs"
m.DB_PATH     = TMP / "data" / "srm.db"
m.ADMIN_API_KEY = "test-key"
m.init_db()

from fastapi.testclient import TestClient
client = TestClient(m.app)

ADMIN = {"x-admin-key": "test-key"}


def _gz(payload: bytes) -> bytes:
    """Wrap raw bytes in a real gzip stream (1F 8B magic + DEFLATE)."""
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb") as g:
        g.write(payload)
    return buf.getvalue()


def _enroll(mac: str, vin: str = None, paid: bool = True, revoked: bool = False) -> str:
    """Enroll a device, optionally set its license state, return its token."""
    body = {"mac": mac}
    if vin:
        body["vin"] = vin
    r = client.post("/admin/devices", json=body, headers=ADMIN)
    assert r.status_code == 200, r.text
    token = r.json()["auth_token"]
    if paid or revoked:
        lic = {"paid": 1 if paid else 0}
        if revoked:
            lic["revoked"] = 1
            lic["revoked_reason"] = "test"
        r = client.post(f"/admin/devices/{mac}/license", json=lic, headers=ADMIN)
        assert r.status_code == 200, r.text
    return token


SAMPLE_CSV = (
    b"timestamp_ms,nmot_w,InjSys_ratEthPrtnBascFu,Com_stCrCtlPan,rl_w,tmot,wdkba\n"
    b"100,5400,50.0,1,95.2,89.0,98.0\n"
    b"200,5800,50.0,1,97.1,89.2,99.6\n"
)


def test_telemetry_upload_happy_path():
    token = _enroll("60:00:00:00:00:01", vin="WAUZZZ4M9PA060001", paid=True)
    gzbody = _gz(SAMPLE_CSV)
    r = client.post(
        "/api/v1/telemetry/log",
        content=gzbody,
        headers={"X-Device-Auth": token, "Content-Type": "application/gzip"},
    )
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["ok"] is True
    assert isinstance(j["log_id"], int) and j["log_id"] >= 1

    # row landed in wot_logs with server-trusted mac/vin + byte_count
    conn = m.db()
    row = conn.execute(
        "SELECT * FROM wot_logs WHERE id = ?", (j["log_id"],)
    ).fetchone()
    conn.close()
    assert row is not None
    assert row["mac"] == "60:00:00:00:00:01"
    assert row["vin"] == "WAUZZZ4M9PA060001"
    assert row["byte_count"] == len(gzbody)

    # file landed on disk under WOT_LOG_DIR and round-trips
    fpath = m.WOT_LOG_DIR / row["file_path"]
    assert fpath.exists()
    assert gzip.decompress(fpath.read_bytes()) == SAMPLE_CSV


def test_telemetry_missing_auth_401():
    gzbody = _gz(SAMPLE_CSV)
    r = client.post("/api/v1/telemetry/log", content=gzbody,
                    headers={"Content-Type": "application/gzip"})
    assert r.status_code == 401


def test_telemetry_bad_token_401():
    gzbody = _gz(SAMPLE_CSV)
    r = client.post("/api/v1/telemetry/log", content=gzbody,
                    headers={"X-Device-Auth": "not-a-real-token"})
    assert r.status_code == 401


def test_telemetry_unpaid_gate_403():
    token = _enroll("60:00:00:00:00:02", vin="WAUZZZ4M9PA060002", paid=False)
    gzbody = _gz(SAMPLE_CSV)
    r = client.post("/api/v1/telemetry/log", content=gzbody,
                    headers={"X-Device-Auth": token})
    assert r.status_code == 403


def test_telemetry_revoked_gate_403():
    token = _enroll("60:00:00:00:00:03", vin="WAUZZZ4M9PA060003",
                    paid=True, revoked=True)
    gzbody = _gz(SAMPLE_CSV)
    r = client.post("/api/v1/telemetry/log", content=gzbody,
                    headers={"X-Device-Auth": token})
    assert r.status_code == 403


def test_telemetry_oversized_413():
    token = _enroll("60:00:00:00:00:04", paid=True)
    # gzip of a large incompressible-ish payload that still exceeds the
    # 64 KB cap once wrapped. Use random bytes so gzip can't shrink it.
    big = _gz(os.urandom(80 * 1024))
    assert len(big) > m.WOT_LOG_MAX_BYTES
    r = client.post("/api/v1/telemetry/log", content=big,
                    headers={"X-Device-Auth": token})
    assert r.status_code == 413


def test_telemetry_non_gzip_400():
    token = _enroll("60:00:00:00:00:05", paid=True)
    r = client.post("/api/v1/telemetry/log", content=b"not gzip at all",
                    headers={"X-Device-Auth": token})
    assert r.status_code == 400


def test_telemetry_empty_body_400():
    token = _enroll("60:00:00:00:00:06", paid=True)
    r = client.post("/api/v1/telemetry/log", content=b"",
                    headers={"X-Device-Auth": token})
    assert r.status_code == 400
