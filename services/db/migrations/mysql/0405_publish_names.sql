-- 0405_publish_names (MySQL / MariaDB): a mod name and an asset name keep 500
-- characters, the publish boxes' maxlength (deob-10.0.7.js:220959, editor.deob.js:20615).
-- The columns were VARCHAR(128). live_name is generated from mod_name, so it is rebuilt.
ALTER TABLE published_mods DROP INDEX published_mods_name_lower_uq;
ALTER TABLE published_mods DROP COLUMN live_name;
ALTER TABLE published_mods MODIFY mod_name VARCHAR(500) NOT NULL;
ALTER TABLE published_mods ADD COLUMN live_name VARCHAR(500) AS (CASE WHEN deleted_at IS NULL THEN mod_name END) STORED;
CREATE UNIQUE INDEX published_mods_name_lower_uq ON published_mods (live_name);
ALTER TABLE published_assets DROP INDEX published_assets_creator_name_idx;
ALTER TABLE published_assets MODIFY name VARCHAR(500) NOT NULL;
CREATE INDEX published_assets_creator_name_idx ON published_assets (creator_id, name);
