-- 20260529_user_vehicle_license.sql
-- PHASE 1 (cloud customer dispatch): customer account model.
-- Adds: users, sessions, vehicles, licenses, stripe_events.
--
-- Apply to an already-live srm.db once:
--   sqlite3 /path/to/data/srm.db < 20260529_user_vehicle_license.sql
-- A fresh deploy needs no manual step — src/main.py::init_db() runs the
-- same CREATE ... IF NOT EXISTS at startup (mirror these two in lockstep;
-- the Dockerfile copies only src/, so init_db() is the container path and
-- this file is the auditable record + already-live-DB path).
--
-- Idempotent: every statement is IF NOT EXISTS, safe to re-run.
-- Reversible: see 20260529_user_vehicle_license.down.sql (DESTRUCTIVE —
--   drops these tables; never run against a populated DB without sign-off).
--
-- Conventions (handoff PHASE 1 schema-level rules):
--   * FK on every join column; ON DELETE explicit.
--       sessions.user_id   -> users(id)      ON DELETE CASCADE
--       vehicles.user_id   -> users(id)      ON DELETE CASCADE
--       vehicles.dongle_mac-> devices(mac)   ON DELETE RESTRICT  (can't drop a claimed dongle)
--       licenses.vehicle_id-> vehicles(id)   ON DELETE RESTRICT
--     Note: CASCADE(vehicles->users) + RESTRICT(licenses->vehicles) means a
--     hard DELETE of a user with licensed vehicles is BLOCKED by the RESTRICT.
--     Intended: normal flow soft-deletes vehicles + revokes licenses, never
--     hard-deletes. The FKs are guardrails against accidental hard deletes.
--   * Indexes on every WHERE/JOIN column we actually use (SQLite does NOT
--     auto-index FK columns).
--   * Timestamps: ISO-8601 UTC TEXT, fixed width 'YYYY-MM-DDTHH:MM:SSZ', so
--     lexicographic comparison == chronological. (The legacy device tables
--     use INTEGER unix ts; the new customer tables standardize on ISO-8601
--     per the handoff "UTC, ISO-8601 in API responses" rule — no cross-type
--     comparison occurs between old and new tables.)

PRAGMA foreign_keys = ON;

-- Customer accounts. email stored lowercased by the app layer; password_hash
-- is argon2id (PHASE 2) — never plaintext/MD5/SHA1.
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    email         TEXT    NOT NULL UNIQUE,
    password_hash TEXT    NOT NULL,
    verified      INTEGER NOT NULL DEFAULT 0,
    created_at    TEXT    NOT NULL
);

-- Server-side sessions (handoff D3). token is the value carried in the
-- HttpOnly/Secure/SameSite=Lax cookie. Revoke = DELETE the row.
CREATE TABLE IF NOT EXISTS sessions (
    token      TEXT    PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at TEXT    NOT NULL,
    created_at TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_user    ON sessions(user_id);     -- revoke-all on reset/logout-all
CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);  -- expiry sweep

-- User <-> vehicle. dongle_mac hard-FKs the existing devices table (the
-- "marry" step, PHASE 3 claim flow). nickname + deleted_at are added now
-- (vs the handoff's listed column set) because PHASE 3 requires PATCH
-- {nickname} and soft-delete; including them here avoids a PHASE 3 ALTER.
CREATE TABLE IF NOT EXISTS vehicles (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL REFERENCES users(id)     ON DELETE CASCADE,
    vin         TEXT    NOT NULL,
    dongle_mac  TEXT    NOT NULL REFERENCES devices(mac)  ON DELETE RESTRICT,
    ecu_boxcode TEXT,
    nickname    TEXT,
    created_at  TEXT    NOT NULL,
    deleted_at  TEXT
);
-- VIN unique only among ACTIVE (non-deleted) vehicles: a sold car's VIN can
-- be re-claimed once the prior owner removes it (PHASE 3 collision handling).
CREATE UNIQUE INDEX IF NOT EXISTS idx_vehicles_vin_active
    ON vehicles(vin) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_vehicles_user ON vehicles(user_id);    -- list a user's vehicles
CREATE INDEX IF NOT EXISTS idx_vehicles_mac  ON vehicles(dongle_mac); -- marry/lookup by MAC

-- Per-vehicle license. The CUSTOMER/BILLING source of truth (handoff D1
-- option A): a reconcile fn (PHASE 3, licenses.py) projects effective state
-- onto the legacy devices.paid/revoked columns the dongle reads, so the
-- /api/v1/device/* + /api/v1/license wire contracts stay untouched (R4).
-- tier + state are TEXT (no CHECK) so values can evolve; the legal value set
-- and state-machine transitions are enforced in licenses.py via named
-- constants (project Rule 3). token is UNIQUE but nullable — many un-issued
-- licenses (NULL) coexist; SQLite treats NULLs as distinct in UNIQUE.
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
CREATE INDEX IF NOT EXISTS idx_licenses_vehicle     ON licenses(vehicle_id);          -- vehicle detail
CREATE INDEX IF NOT EXISTS idx_licenses_state_until ON licenses(state, valid_until);  -- expiry sweep

-- Stripe webhook ingest — STUB for now (no real Stripe wiring; Sean owns
-- the keys). event_id is the Stripe 'evt_...' id, UNIQUE for idempotent
-- ingest; processed_at NULL until handled.
CREATE TABLE IF NOT EXISTS stripe_events (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id     TEXT    UNIQUE,
    payload_json TEXT    NOT NULL,
    received_at  TEXT    NOT NULL,
    processed_at TEXT
);
