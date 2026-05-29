"""
Shared pytest fixtures for the cloud suite.

`client` is FUNCTION-SCOPED and uses pytest's `monkeypatch` + `tmp_path`,
so every test gets a fresh DB + temp dirs that are torn down (and the
module globals restored) after it returns. This is the AUDIT-C4 fix: the
older suites (test_smoke.py, test_telemetry_log.py) monkeypatch
`src.main` globals at IMPORT time and only coexist by using disjoint MAC
namespaces — adding auth tests that need a seeded `users`/`sessions` DB
would collide with that. Tests that opt into THIS fixture are isolated
and cannot leak DB_PATH into the import-time suites (the fixture only
mutates globals for the duration of the requesting test).

PBKDF2 cost is knocked down to a tiny iteration count here purely for
speed — the production default (600k) would make the auth tests crawl.
"""
import pytest


# Iteration count used only under test. Real default lives in passwords.py.
_TEST_PBKDF2_ITERATIONS = 1000


@pytest.fixture
def client(monkeypatch, tmp_path):
    import src.main as m
    import src.passwords as pw

    data = tmp_path / "data"
    data.mkdir()
    (tmp_path / "firmware").mkdir()
    (tmp_path / "calibrations").mkdir()
    (tmp_path / "wot_logs").mkdir()

    monkeypatch.setattr(m, "DATA_DIR", data)
    monkeypatch.setattr(m, "FW_DIR", tmp_path / "firmware")
    monkeypatch.setattr(m, "CAL_DIR", tmp_path / "calibrations")
    monkeypatch.setattr(m, "WOT_LOG_DIR", tmp_path / "wot_logs")
    monkeypatch.setattr(m, "DB_PATH", data / "srm.db")
    monkeypatch.setattr(m, "ADMIN_API_KEY", "test-key")
    # TestClient speaks http, so a Secure cookie would be dropped — turn it
    # off so the session cookie round-trips in tests.
    monkeypatch.setattr(m, "SESSION_COOKIE_SECURE", False)
    monkeypatch.setattr(pw, "PBKDF2_ITERATIONS", _TEST_PBKDF2_ITERATIONS)

    m.init_db()

    from fastapi.testclient import TestClient
    with TestClient(m.app) as c:
        yield c
