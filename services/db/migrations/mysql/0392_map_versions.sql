-- 0392_map_versions (MySQL / MariaDB): same as postgres/0392. Krunker v10.0.7's map versions behind kn_brain
-- (services/brain/v10_catalog.c): the map data of each live version of a published map. Additive only.
CREATE TABLE map_versions (
    map_id     BIGINT NOT NULL,
    version    BIGINT NOT NULL,
    map_data   MEDIUMTEXT NOT NULL,
    data_size  BIGINT NOT NULL DEFAULT 0,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (map_id, version),
    FOREIGN KEY (map_id) REFERENCES published_maps(map_id) ON DELETE CASCADE
);

INSERT INTO map_versions (map_id, version, map_data, data_size, created_at)
SELECT map_id, map_updatecounter, map_data, LENGTH(map_data), map_date FROM published_maps WHERE deleted_at IS NULL;
