-- 0405_publish_names (PostgreSQL): mod_name and name are already text. The cap is 500
-- characters, the publish boxes' maxlength (deob-10.0.7.js:220959, editor.deob.js:20615).
COMMENT ON COLUMN published_mods.mod_name IS 'at most 500 characters';
COMMENT ON COLUMN published_assets.name IS 'at most 500 characters';
