-- 0207_mm_ruleset: the Krunker version a lobby plays (server_bin --ruleset, shared/include/ruleset.h:
-- "v7" / "v10"; doc/v10/version-toggle.md). Additive: rows of servers from before the toggle are v7.
-- The brain reads it from the lobby tuple's meta.ruleset; the column is for queries and dashboards.
ALTER TABLE mm_lobbies ADD COLUMN ruleset text NOT NULL DEFAULT 'v7';
