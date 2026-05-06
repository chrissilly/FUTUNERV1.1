"""
Smoke tests. Run with: PYTHONPATH=. pytest tests/

These exercise every endpoint without an actual dongle. Set up a
temp DB + admin key, hit the endpoints, assert the shapes and auth
behavior.
"""
import os, sys, tempfile, struct, hashlib
from pathlib import Path
import pytest

# point the app at a fresh tempdir BEFORE importing
TMP = Path(tempfile.mkdtemp(prefix="srm-test-"))
(TMP / "data").mkdir(); (TMP / "firmware").mkdir(); (TMP / "calibrations").mkdir()

# Force the app to use our temp directories by monkeypatching paths.
import src.main as m
m.DATA_DIR = TMP / "data"
m.FW_DIR   = TMP / "firmware"
m.CAL_DIR  = TMP / "calibrations"
m.DB_PATH  = TMP / "data" / "srm.db"
m.ADMIN_API_KEY = "test-key"
m.init_db()

from fastapi.testclient import TestClient
client = TestClient(m.app)

ADMIN = {"x-admin-key": "test-key"}


def test_root():
    r = client.get("/")
    assert r.status_code == 200
    assert r.json()["service"] == "SRM Cloud"

def test_admin_auth():
    r = client.get("/admin/devices")          # no key
    assert r.status_code == 403
    r = client.get("/admin/devices", headers=ADMIN)
    assert r.status_code == 200
    assert r.json() == []

def test_enroll_then_register():
    r = client.post("/admin/devices",
        json={"mac": "30:ed:a0:b6:35:40", "vin":"WUAPCBF28NN902533",
              "boxcode":"4K0907557G__0003", "ethanol_random": 0x2AF},
        headers=ADMIN)
    assert r.status_code == 200
    token = r.json()["auth_token"]
    assert len(token) == 32

    # bad token
    r = client.post("/api/v1/device/register",
        json={"mac":"30:ed:a0:b6:35:40"},
        headers={"authorization":"Bearer wrong"})
    assert r.status_code == 401

    # right token
    r = client.post("/api/v1/device/register",
        json={"mac":"30:ed:a0:b6:35:40","build_hash":"abc"},
        headers={"authorization":f"Bearer {token}"})
    assert r.status_code == 200
    assert r.json()["ok"] is True

def test_firmware_flow():
    # enroll
    r = client.post("/admin/devices",
        json={"mac":"aa:bb:cc:dd:ee:ff"}, headers=ADMIN)
    token = r.json()["auth_token"]

    # upload firmware
    fake = b"FAKEFW" + b"\x00"*100
    r = client.put("/admin/firmware/abc1234?build_number=1&is_release=1",
        files={"file": ("abc1234.bin", fake, "application/octet-stream")},
        headers=ADMIN)
    assert r.status_code == 200, r.text

    # device queries — gets the release because nothing is assigned
    r = client.get("/api/v1/device/update_available",
        headers={"authorization":f"Bearer {token}"})
    j = r.json()
    assert j["available"] == 1
    assert j["build_hash"] == "abc1234"

    # download
    r = client.get(j["url"], headers={"authorization":f"Bearer {token}"})
    assert r.status_code == 200
    assert r.content == fake

    # if the dongle says "I'm already on abc1234", server says no update
    r = client.get("/api/v1/device/update_available?current_hash=abc1234",
        headers={"authorization":f"Bearer {token}"})
    assert r.json()["available"] == 0

def test_register_409_on_vin_mismatch():
    r = client.post("/admin/devices",
        json={"mac":"40:00:00:00:00:01", "vin":"WAUZZZ4M9PA000001"},
        headers=ADMIN)
    token = r.json()["auth_token"]

    # Different VIN under same token → 409.
    r = client.post("/api/v1/device/register",
        json={"mac":"40:00:00:00:00:01", "vin":"WAUZZZ4M9PA999999"},
        headers={"authorization": f"Bearer {token}"})
    assert r.status_code == 409
    body = r.json()
    assert "WAUZZZ4M9PA000001" in body["detail"]
    assert "WAUZZZ4M9PA999999" in body["detail"]

def test_register_idempotent_when_vin_matches_normalized():
    r = client.post("/admin/devices",
        json={"mac":"40:00:00:00:00:02", "vin":"WAUZZZ4M9PA000002"},
        headers=ADMIN)
    token = r.json()["auth_token"]

    # Same VIN with leading whitespace + lowercase → normalize-equal.
    r = client.post("/api/v1/device/register",
        json={"mac":"40:00:00:00:00:02", "vin":"  wauzzz4m9pa000002 "},
        headers={"authorization": f"Bearer {token}"})
    assert r.status_code == 200
    assert r.json()["ok"] is True

def test_register_first_set_when_existing_null():
    # Enroll without a VIN.
    r = client.post("/admin/devices",
        json={"mac":"40:00:00:00:00:03"}, headers=ADMIN)
    token = r.json()["auth_token"]

    # First register with a VIN sets it; no 409.
    r = client.post("/api/v1/device/register",
        json={"mac":"40:00:00:00:00:03", "vin":"WAUZZZ4M9PA000003"},
        headers={"authorization": f"Bearer {token}"})
    assert r.status_code == 200

    # Subsequent register with same VIN is idempotent.
    r = client.post("/api/v1/device/register",
        json={"mac":"40:00:00:00:00:03", "vin":"WAUZZZ4M9PA000003"},
        headers={"authorization": f"Bearer {token}"})
    assert r.status_code == 200

def test_register_skip_when_no_vin_in_body():
    r = client.post("/admin/devices",
        json={"mac":"40:00:00:00:00:04", "vin":"WAUZZZ4M9PA000004"},
        headers=ADMIN)
    token = r.json()["auth_token"]

    # No VIN in body → no comparison, no 409, existing VIN preserved.
    r = client.post("/api/v1/device/register",
        json={"mac":"40:00:00:00:00:04", "boxcode":"4K0907557G__0003"},
        headers={"authorization": f"Bearer {token}"})
    assert r.status_code == 200

def test_license_endpoint_default_unpaid():
    r = client.post("/admin/devices",
        json={"mac":"50:00:00:00:00:01", "vin":"WAUZZZ4M9PA005001"},
        headers=ADMIN)
    token = r.json()["auth_token"]

    r = client.get("/api/v1/license",
        headers={"authorization": f"Bearer {token}"})
    assert r.status_code == 200
    body = r.json()
    assert body["paid"] is False
    assert body["revoked"] is False
    assert body["revoked_reason"] in (None, "")
    assert body["vin"] == "WAUZZZ4M9PA005001"

def test_license_endpoint_after_admin_set_paid():
    r = client.post("/admin/devices",
        json={"mac":"50:00:00:00:00:02", "vin":"WAUZZZ4M9PA005002"},
        headers=ADMIN)
    token = r.json()["auth_token"]

    r = client.post("/admin/devices/50:00:00:00:00:02/license",
        json={"paid": 1}, headers=ADMIN)
    assert r.status_code == 200, r.text

    r = client.get("/api/v1/license",
        headers={"authorization": f"Bearer {token}"})
    body = r.json()
    assert body["paid"] is True
    assert body["revoked"] is False
    assert body["vin"] == "WAUZZZ4M9PA005002"

def test_license_endpoint_after_admin_revoke():
    r = client.post("/admin/devices",
        json={"mac":"50:00:00:00:00:03", "vin":"WAUZZZ4M9PA005003"},
        headers=ADMIN)
    token = r.json()["auth_token"]

    r = client.post("/admin/devices/50:00:00:00:00:03/license",
        json={"paid": 1, "revoked": 1, "revoked_reason": "chargeback"},
        headers=ADMIN)
    assert r.status_code == 200, r.text

    r = client.get("/api/v1/license",
        headers={"authorization": f"Bearer {token}"})
    body = r.json()
    assert body["paid"] is True
    assert body["revoked"] is True
    assert body["revoked_reason"] == "chargeback"

def test_license_endpoint_unauthenticated():
    r = client.get("/api/v1/license")  # no Authorization header
    assert r.status_code == 401
    r = client.get("/api/v1/license",
        headers={"authorization": "Bearer not-a-real-token"})
    assert r.status_code == 401

def test_admin_license_set_unknown_device():
    r = client.post("/admin/devices/aa:99:88:77:66:55/license",
        json={"paid": 1}, headers=ADMIN)
    assert r.status_code == 404


def test_calibration_with_ethanol_patch():
    # enroll device with a specific ethanol_random
    r = client.post("/admin/devices",
        json={"mac":"01:02:03:04:05:06", "ethanol_random": 0x123},
        headers=ADMIN)
    token = r.json()["auth_token"]

    # craft a fake SBF: 'SCPN' + 11 u32 + some payload
    header = b"SCPN" + struct.pack("<11I",
        5,           # version
        1,           # segments
        0,           # inverse_segments
        0,           # maps
        48,          # segment_start
        0,           # _reserved_a
        0,           # inverse_segment_start
        0,           # map_start
        0,           # _reserved_b
        10,          # ethanol_bit_count
        0x999,       # ethanol_random — base value, will be replaced per-device
    )
    payload = header + b"\x00" * 64

    # upload
    r = client.post("/admin/calibrations/test.sbf?boxcode=4K0907557G__0003",
        files={"file": ("test.sbf", payload, "application/octet-stream")},
        headers=ADMIN)
    assert r.status_code == 200, r.text

    # assign
    r = client.post("/admin/devices/01:02:03:04:05:06/assign_calibration?filename=test.sbf",
        headers=ADMIN)
    assert r.status_code == 200

    # device pulls it down → ethanol_random should be patched to 0x123
    r = client.get("/api/v1/device/calibration",
        headers={"authorization":f"Bearer {token}"})
    assert r.status_code == 200
    out = r.content
    patched_eth = struct.unpack_from("<I", out, m.SCPN_HEADER_OFFSET_ETHANOL_RANDOM)[0]
    assert patched_eth == 0x123, f"expected per-device ethanol_random, got {patched_eth:x}"
