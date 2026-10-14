-- 0113_clan_v10 (SQLite): same columns as postgres/0113_clan_v10 (see there).
ALTER TABLE clans ADD COLUMN clan_logo          INTEGER NOT NULL DEFAULT 0;
ALTER TABLE clans ADD COLUMN clan_logo_pending  INTEGER NOT NULL DEFAULT 0;
ALTER TABLE clans ADD COLUMN clan_banner_color  TEXT;
ALTER TABLE clans ADD COLUMN clan_motd_by       INTEGER REFERENCES accounts(id) ON DELETE SET NULL;
ALTER TABLE clans ADD COLUMN clan_motd_at       TIMESTAMP;
ALTER TABLE clans ADD COLUMN clan_transfer_to   INTEGER REFERENCES accounts(id) ON DELETE SET NULL;
ALTER TABLE clans ADD COLUMN clan_transfer_at   TIMESTAMP;
