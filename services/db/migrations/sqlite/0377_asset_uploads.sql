-- 0377_asset_uploads (SQLite): same columns as postgres/0377_asset_uploads (see there).
ALTER TABLE published_assets ADD COLUMN is_private INTEGER NOT NULL DEFAULT 0;
ALTER TABLE published_assets ADD COLUMN version INTEGER NOT NULL DEFAULT 1;
ALTER TABLE published_assets ADD COLUMN updated_at TIMESTAMP;
ALTER TABLE published_assets ADD COLUMN uvc TEXT;
CREATE INDEX published_assets_creator_name_idx ON published_assets (creator_id, lower(name)) WHERE deleted_at IS NULL;
-- AUTOINCREMENT: the next id is sqlite_sequence's seq + 1 (the row exists once the table had an insert)
INSERT INTO sqlite_sequence (name, seq)
    SELECT 'published_assets', 999999999 WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = 'published_assets');
UPDATE sqlite_sequence SET seq = max(seq, 999999999) WHERE name = 'published_assets';
