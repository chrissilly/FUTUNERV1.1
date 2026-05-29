"""
PHASE 2 schema-migration tests for the auth-tokens table (user_tokens).

Same shape as test_migration_user_vehicle.py: the first three tests are
self-contained — they apply the raw migrations/20260529_user_auth_tokens
{,.down}.sql to a throwaway sqlite with NO app import (immune to the
import-time monkeypatching the other suites do, AUDIT C4). The fourth
imports the app and runs init_db() under a function-scoped monkeypatch,
asserting the fresh-deploy path (the only path the Dockerfile ships)
creates the SAME object as the migration — the "edit BOTH or neither"
lockstep written into both the .sql header and init_db().

user_tokens FKs users(id), so each test seeds a minimal `users` stub
first (mirrors the documented apply ORDER: vehicle/license migration,
then this one).
"""
import sqlite3
from pathlib import Path

MIGRATIONS = Path(__file__).resolve().parent.parent / "migrations"
UP_SQL   = (MIGRATIONS / "20260529_user_auth_tokens.sql").read_text()
DOWN_SQL = (MIGRATIONS / "20260529_user_auth_tokens.down.sql").read_text()

PHASE2_TABLE = "user_tokens"
PHASE2_INDEX = "idx_user_tokens_user"


def _names(conn, kind):
    rows = conn.execute(
        "SELECT name FROM sqlite_master WHERE type = ?", (kind,)
    ).fetchall()
    return {r[0] for r in rows}


def _fresh_db_with_users(tmp_path):
    """File-backed sqlite holding only a minimal users stub that
    user_tokens.user_id FKs."""
    conn = sqlite3.connect(tmp_path / "srm.db")
    conn.execute("CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT)")
    conn.commit()
    return conn


def test_up_creates_table_and_index(tmp_path):
    conn = _fresh_db_with_users(tmp_path)
    conn.executescript(UP_SQL)
    assert PHASE2_TABLE in _names(conn, "table")
    assert PHASE2_INDEX in _names(conn, "index")
    conn.close()


def test_up_is_idempotent(tmp_path):
    conn = _fresh_db_with_users(tmp_path)
    conn.executescript(UP_SQL)
    conn.executescript(UP_SQL)   # re-run must not raise: every stmt is IF NOT EXISTS
    assert PHASE2_TABLE in _names(conn, "table")
    conn.close()


def test_down_drops_only_user_tokens(tmp_path):
    conn = _fresh_db_with_users(tmp_path)
    conn.executescript(UP_SQL)
    conn.executescript(DOWN_SQL)
    tables = _names(conn, "table")
    assert PHASE2_TABLE not in tables   # removed
    assert "users" in tables            # FK parent left untouched
    conn.close()


def test_init_db_creates_user_tokens(monkeypatch, tmp_path):
    """Lockstep: the fresh-deploy path (init_db) must create the same
    PHASE 2 object as the migration file."""
    import src.main as m
    monkeypatch.setattr(m, "DB_PATH", tmp_path / "srm.db")
    m.init_db()
    conn = sqlite3.connect(tmp_path / "srm.db")
    assert PHASE2_TABLE in _names(conn, "table")
    assert PHASE2_INDEX in _names(conn, "index")
    conn.close()
