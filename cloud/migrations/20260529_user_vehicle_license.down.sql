-- 20260529_user_vehicle_license.down.sql
-- REVERSE of 20260529_user_vehicle_license.sql.
--
-- ############################################################################
-- ## DESTRUCTIVE. Drops users, sessions, vehicles, licenses, stripe_events. ##
-- ## On a populated DB this deletes ALL customer accounts, vehicle claims,  ##
-- ## and license state. Lossless ONLY on an empty/fresh DB. NEVER run       ##
-- ## against production without an explicit, signed-off backup + restore    ##
-- ## plan. The legacy device tables (devices/firmware/calibrations/         ##
-- ## device_log/wot_logs) are untouched — this only removes PHASE 1 tables. ##
-- ############################################################################
--
-- Usage (only after sign-off):
--   sqlite3 /path/to/data/srm.db < 20260529_user_vehicle_license.down.sql
--
-- foreign_keys = OFF so drop order can't be blocked by the
-- licenses->vehicles->users RESTRICT/CASCADE chain. We still drop
-- child-before-parent for clarity. The partial/secondary indexes are
-- dropped automatically by SQLite when their table is dropped, so they
-- need no explicit DROP INDEX.

PRAGMA foreign_keys = OFF;

DROP TABLE IF EXISTS licenses;
DROP TABLE IF EXISTS sessions;
DROP TABLE IF EXISTS vehicles;
DROP TABLE IF EXISTS users;
DROP TABLE IF EXISTS stripe_events;
