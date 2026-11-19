-- 0377_asset_uploads (PostgreSQL): the editor's Publish Asset on a Box Office server (services/brain/asset_upload.c,
-- uploadA -> pubR -> POST /upload-user-asset; editor-10.0.7.js:20656-20806).
--   is_private  the popup's Private switch (gamePrivNew): left out of the other players' asset lists
--   version     1 on publish, + 1 on each update of the same asset (same creator and name)
--   updated_at  the last update (NULL: never updated)
--   uvc         the per-map UV channels of a model ({letter: 1..7}, the popup's Advanced rows), or NULL;
--               kept out of data: data's keys are the asset id's letters (MapManager.addAsset)
-- New ids start at 1000000000 (shared/include/user_asset_path.h USER_ASSET_KN_ID_BASE), far above Krunker's
-- user-asset ids, so a Box Office asset never names one of Krunker's files.
ALTER TABLE published_assets ADD COLUMN is_private boolean NOT NULL DEFAULT false;
ALTER TABLE published_assets ADD COLUMN version integer NOT NULL DEFAULT 1;
ALTER TABLE published_assets ADD COLUMN updated_at timestamptz;
ALTER TABLE published_assets ADD COLUMN uvc jsonb;
CREATE INDEX published_assets_creator_name_idx ON published_assets (creator_id, lower(name)) WHERE deleted_at IS NULL;
SELECT setval(pg_get_serial_sequence('published_assets', 'id'),
              GREATEST(999999999, (SELECT coalesce(max(id), 0) FROM published_assets)));
