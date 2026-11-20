-- 0392_map_versions (SQLite): same as postgres/0392. Krunker v10.0.7's map versions behind kn_brain
-- (services/brain/v10_catalog.c): the map data of each live version of a published map. Additive only.
CREATE TABLE map_versions (
    map_id     INTEGER NOT NULL REFERENCES published_maps(map_id) ON DELETE CASCADE,
    version    INTEGER NOT NULL,
    map_data   TEXT NOT NULL,
    data_size  INTEGER NOT NULL DEFAULT 0,
    created_at TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (map_id, version)
);

INSERT INTO map_versions (map_id, version, map_data, data_size, created_at)
SELECT map_id, map_updatecounter, map_data, length(CAST(map_data AS BLOB)), map_date FROM published_maps WHERE deleted_at IS NULL;
