-- 0403_map_mobile (SQLite): a published map's Mobile Friendly flag (the editor's Publish Game popup, uploadM's last
-- argument, editor-10.0.7.js:20567, :20636-20646), sent back as the editor's plm row's mob when the map is updated
-- (publishGamePopup, :20562-20567; creations.c). Additive: old rows read 0.
ALTER TABLE published_maps ADD COLUMN map_mobile INTEGER NOT NULL DEFAULT 0;
