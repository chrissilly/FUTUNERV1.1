-- 20260529_user_auth_tokens.sql
-- PHASE 2 (cloud customer dispatch): single-use email-link tokens.
-- Adds: user_tokens.
--
-- ORDER: apply AFTER 20260529_user_vehicle_license.sql — user_tokens FKs
-- users(id), so that table must already exist.
--
-- Apply to an already-live srm.db once:
--   sqlite3 /path/to/data/srm.db < 20260529_user_auth_tokens.sql
-- A fresh deploy needs no manual step — src/main.py::init_db() runs the same
-- CREATE ... IF NOT EXISTS at startup (mirror these two in lockstep; the
-- Dockerfile copies only src/, so init_db() is the container path and this
-- file is the auditable record + already-live-DB path).
--
-- Idempotent: every statement is IF NOT EXISTS, safe to re-run.
-- Reversible: see 20260529_user_auth_tokens.down.sql (DESTRUCTIVE — drops the
--   table; never run against a populated DB without sign-off).
--
-- Why a separate table from `sessions`:
--   * sessions = login state (carried in the cookie, swept on expiry/logout).
--   * user_tokens = out-of-band links the STUBBED email would carry (account
--     verification + password reset). Different lifetime, different lifecycle.
--   `purpose` ('verify' | 'reset') separates the two; `used_at` makes a token
--   single-use (set on redemption, then it can never be replayed). Legal
--   `purpose` values + TTLs live in src/main.py as named constants (Rule 3),
--   not as a CHECK here, so they can evolve without a schema change.
--   Timestamps: ISO-8601 UTC TEXT (same convention as the PHASE 1 tables).

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS user_tokens (
    token      TEXT    PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    purpose    TEXT    NOT NULL,
    expires_at TEXT    NOT NULL,
    created_at TEXT    NOT NULL,
    used_at    TEXT
);
CREATE INDEX IF NOT EXISTS idx_user_tokens_user ON user_tokens(user_id);  -- redeem/sweep by user
