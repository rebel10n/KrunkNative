-- 0377_asset_uploads (MySQL): same columns as postgres/0377_asset_uploads (see there).
ALTER TABLE published_assets ADD COLUMN is_private TINYINT NOT NULL DEFAULT 0;
ALTER TABLE published_assets ADD COLUMN version BIGINT NOT NULL DEFAULT 1;
ALTER TABLE published_assets ADD COLUMN updated_at DATETIME(3) NULL;
ALTER TABLE published_assets ADD COLUMN uvc JSON NULL;
CREATE INDEX published_assets_creator_name_idx ON published_assets (creator_id, name);
-- AUTO_INCREMENT never goes below the table's max(id) + 1
ALTER TABLE published_assets AUTO_INCREMENT = 1000000000;
