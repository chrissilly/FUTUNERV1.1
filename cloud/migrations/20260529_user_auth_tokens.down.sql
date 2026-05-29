-- 20260529_user_auth_tokens.down.sql
-- REVERSE of 20260529_user_auth_tokens.sql.
--
-- ############################################################################
-- ## DESTRUCTIVE. Drops user_tokens. On a populated DB this invalidates ALL  ##
-- ## outstanding email-verification + password-reset links (rows gone). It   ##
-- ## does NOT touch users/sessions, so no account or login state is lost —   ##
-- ## but any link already mailed out stops working. Lossless on an empty/    ##
-- ## fresh DB. NEVER run against production without sign-off.                 ##
-- ############################################################################
--
-- ORDER: run this BEFORE 20260529_user_vehicle_license.down.sql — that
-- reverse drops users, which user_tokens FKs. (Reverse the apply order.)
--
-- Usage (only after sign-off):
--   sqlite3 /path/to/data/srm.db < 20260529_user_auth_tokens.down.sql
--
-- idx_user_tokens_user is dropped automatically with its table.

PRAGMA foreign_keys = OFF;

DROP TABLE IF EXISTS user_tokens;
