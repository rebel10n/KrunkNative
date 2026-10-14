-- 0113_clan_v10: Krunker v10.0.2's clan window extras (client/src/ui/clan_ui.c, services/brain/
-- clan_v10.c): the settings tab's logo versions, banner colour (a paint), message-of-the-day
-- author / time and the pending ownership transfer. Only new nullable / defaulted columns on
-- clans (0101), safe on the live database. clan_discord (0101) holds the Discord invite code
-- (v10 clan_link4), clan_motd (0101) the message itself.
ALTER TABLE clans ADD COLUMN clan_logo          integer NOT NULL DEFAULT 0;  -- approved logo version, 0 = none
ALTER TABLE clans ADD COLUMN clan_logo_pending  integer NOT NULL DEFAULT 0;  -- version waiting for review, 0 = none
ALTER TABLE clans ADD COLUMN clan_banner_color  text;                         -- '#rrggbb' or 's<shader>'
ALTER TABLE clans ADD COLUMN clan_motd_by       integer REFERENCES accounts(id) ON DELETE SET NULL;
ALTER TABLE clans ADD COLUMN clan_motd_at       timestamptz;
ALTER TABLE clans ADD COLUMN clan_transfer_to   integer REFERENCES accounts(id) ON DELETE SET NULL;
ALTER TABLE clans ADD COLUMN clan_transfer_at   timestamptz;
