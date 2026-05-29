"""
PHASE 1 schema-migration tests for the customer model
(users / sessions / vehicles / licenses / stripe_events).

Run with: PYTHONPATH=. pytest tests/test_migration_user_vehicle.py

The first three tests are fully self-contained — they apply the raw
migrations/20260529_user_vehicle_license{,.down}.sql to a throwaway
sqlite file with NO app import, so they cannot be perturbed by the
module-global monkeypatching the other suites do at import time
(AUDIT C4). The fourth test imports the app and runs init_db() under a
*function-scoped* monkeypatch, asserting the fresh-deploy path (the
only path the Dockerfile ships) creates the SAME objects as the
migration file — this guards the "edit BOTH or neither" contract
written into both the .sql header and init_db().
"""
import sqlite3
from pathlib import Path

MIGRATIONS = Path(__file__).resolve().parent.parent / "migrations"
UP_SQL   = (MIGRATIONS / "20260529_user_vehicle_license.sql").read_text()
DOWN_SQL = (MIGRATIONS / "20260529_user_vehicle_license.down.sql").read_text()

# The PHASE 1 objects both paths must create. Listed explicitly (not
# derived from either source) so a renamed/dropped/typo'd object in
# either path fails loudly instead of silently agreeing with a bug.
PHASE1_TABLES = {"users", "sessions", "vehicles", "licenses", "stripe_events"}
PHASE1_INDEXES = {
    "idx_sessions_user", "idx_sessions_expires",
    "idx_vehicles_vin_active", "idx_vehicles_user", "idx_vehicles_mac",
    "idx_licenses_vehicle", "idx_licenses_state_until",
}


def _names(conn, kind):
    rows = conn.execute(
        "SELECT name FROM sqlite_master WHERE type = ?", (kind,)
    ).fetchall()
    return {r[0] for r in rows}


def _fresh_db_with_devices(tmp_path):
    """File-backed sqlite holding only the legacy devices stub that
    vehicles.dongle_mac FKs. File-backed (not :memory:) so the multi-
    statement executescript + PRAGMAs behave exactly as against the
    real srm.db."""
    conn = sqlite3.connect(tmp_path / "srm.db")
    conn.execute("CREATE TABLE devices (mac TEXT PRIMARY KEY)")
    conn.commit()
    return conn


def test_up_creates_tables_and_indexes(tmp_path):
    conn = _fresh_db_with_devices(tmp_path)
    conn.executescript(UP_SQL)
    assert PHASE1_TABLES <= _names(conn, "table")
    assert PHASE1_INDEXES <= _names(conn, "index")
    conn.close()


def test_up_is_idempotent(tmp_path):
    conn = _fresh_db_with_devices(tmp_path)
    conn.executescript(UP_SQL)
    conn.executescript(UP_SQL)   # re-run must not raise: every stmt is IF NOT EXISTS
    assert PHASE1_TABLES <= _names(conn, "table")
    assert PHASE1_INDEXES <= _names(conn, "index")
    conn.close()


def test_down_drops_only_phase1_tables(tmp_path):
    conn = _fresh_db_with_devices(tmp_path)
    conn.executescript(UP_SQL)
    conn.executescript(DOWN_SQL)
    tables = _names(conn, "table")
    assert PHASE1_TABLES.isdisjoint(tables)   # all five removed
    assert "devices" in tables                # legacy table left untouched
    conn.close()


def test_init_db_matches_migration(monkeypatch, tmp_path):
    """Lockstep: the fresh-deploy path must create the same PHASE 1
    objects as the migration file. Uses a function-scoped monkeypatch so
    it cannot leak DB_PATH into the other suites (AUDIT C4)."""
    import src.main as m
    monkeypatch.setattr(m, "DB_PATH", tmp_path / "srm.db")
    m.init_db()
    conn = sqlite3.connect(tmp_path / "srm.db")
    assert PHASE1_TABLES <= _names(conn, "table")
    assert PHASE1_INDEXES <= _names(conn, "index")
    conn.close()
