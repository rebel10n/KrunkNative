-- 0300_ks_storage (MySQL/MariaDB): KrunkScript GAME.STORAGE and the Save / Load Data map
-- triggers. Same table as postgres/0300.
CREATE TABLE ks_storage (
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    map_id      BIGINT NOT NULL REFERENCES published_maps(map_id) ON DELETE CASCADE,
    skey        VARCHAR(64) NOT NULL,
    svalue      VARCHAR(512) NOT NULL,
    scope       SMALLINT NOT NULL DEFAULT 0 CHECK (scope IN (0, 1)),
    updated_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, map_id, skey)
);
CREATE INDEX ks_storage_map_idx ON ks_storage (map_id);
