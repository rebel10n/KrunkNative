-- 0392_map_versions (PostgreSQL): Krunker v10.0.7's map versions (GET /maps/:id/versions, POST
-- /maps/:id/versions/:v/revert, deob-10.0.7.js:26713-26723; the Creations map page's Versions tab, 107794-107835,
-- 108195-108266) behind kn_brain (services/brain/v10_catalog.c). Additive only.
--
-- map_versions: the map data of each live version of a published map (published_maps.map_updatecounter when it went
-- live): every publish, leaderboard update and revert stores the new live data (db_map_snapshot, db_social.c), so the
-- version that was live before a revert stays listed and a revert can be undone. A map keeps its newest
-- MAP_VERSIONS_KEEP versions. The maps published before this migration start with their live version.
CREATE TABLE map_versions (
    map_id     integer NOT NULL REFERENCES published_maps(map_id) ON DELETE CASCADE,
    version    integer NOT NULL,
    map_data   text NOT NULL,
    data_size  integer NOT NULL DEFAULT 0,
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (map_id, version)
);

INSERT INTO map_versions (map_id, version, map_data, data_size, created_at)
SELECT map_id, map_updatecounter, map_data, octet_length(map_data), map_date FROM published_maps WHERE deleted_at IS NULL;
