-- 0300_ks_storage: KrunkScript GAME.STORAGE and the Save / Load Data map triggers
-- (doc/v7/krunkscript.md 2.6, WP6; v7.1.4 procs 154 / 163). One string value per account,
-- published map and key; scope 1 = global (readable by the creator's other maps).
-- Written by kn_brain's ops "storage" / "storage_load" (services/brain/db_storage.c).
CREATE TABLE ks_storage (
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    map_id      integer NOT NULL REFERENCES published_maps(map_id) ON DELETE CASCADE,
    skey        text NOT NULL CHECK (length(skey) BETWEEN 1 AND 64),
    svalue      text NOT NULL CHECK (length(svalue) <= 512),
    scope       smallint NOT NULL DEFAULT 0 CHECK (scope IN (0, 1)),
    updated_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, map_id, skey)
);
CREATE INDEX ks_storage_map_idx ON ks_storage (map_id);
