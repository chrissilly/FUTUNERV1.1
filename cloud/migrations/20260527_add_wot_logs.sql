-- P-67: WOT telemetry log uploads.
--
-- Apply to the existing production srm.db once (the app's init_db()
-- also runs this CREATE TABLE IF NOT EXISTS at startup, so a fresh
-- deploy needs no manual step — this file is for the already-live DB
-- and as the auditable record of the schema change).
--
--   sqlite3 /path/to/data/srm.db < 20260527_add_wot_logs.sql
--
-- One row per uploaded gzip CSV. mac/vin are copied from the
-- authenticating devices row at upload time (server-trusted, never
-- client-claimed). file_path is relative to WOT_LOG_DIR.

CREATE TABLE IF NOT EXISTS wot_logs (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    mac              TEXT NOT NULL,
    vin              TEXT,
    uploaded_at      INTEGER NOT NULL,
    file_path        TEXT NOT NULL,
    byte_count       INTEGER NOT NULL
);

-- Lookups are by device (admin pulling a device's logs newest-first).
CREATE INDEX IF NOT EXISTS idx_wot_logs_mac_ts
    ON wot_logs (mac, uploaded_at DESC);
