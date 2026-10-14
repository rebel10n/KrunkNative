-- 0113_clan_v10 (MySQL): same columns as postgres/0113_clan_v10 (see there).
ALTER TABLE clans ADD COLUMN clan_logo          BIGINT NOT NULL DEFAULT 0;
ALTER TABLE clans ADD COLUMN clan_logo_pending  BIGINT NOT NULL DEFAULT 0;
ALTER TABLE clans ADD COLUMN clan_banner_color  TEXT;
ALTER TABLE clans ADD COLUMN clan_motd_by       BIGINT;
ALTER TABLE clans ADD COLUMN clan_motd_at       DATETIME(3);
ALTER TABLE clans ADD COLUMN clan_transfer_to   BIGINT;
ALTER TABLE clans ADD COLUMN clan_transfer_at   DATETIME(3);
