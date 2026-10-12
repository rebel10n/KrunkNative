-- 0300_ks_storage (SQLite): KrunkScript GAME.STORAGE and the Save / Load Data map triggers.
-- Same table as postgres/0300.
CREATE TABLE ks_storage (
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    map_id      INTEGER NOT NULL REFERENCES published_maps(map_id) ON DELETE CASCADE,
    skey        TEXT NOT NULL CHECK (length(skey) BETWEEN 1 AND 64),
    svalue      TEXT NOT NULL CHECK (length(svalue) <= 512),
    scope       INTEGER NOT NULL DEFAULT 0 CHECK (scope IN (0, 1)),
    updated_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, map_id, skey)
);
CREATE INDEX ks_storage_map_idx ON ks_storage (map_id);
