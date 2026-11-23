-- 0394_custom_skins (SQLite): same as postgres/0394. Krunker v10.0.7's Skin Manager records behind kn_brain
-- (services/brain/skins.c, P_CUSTOM_SKIN). Additive only.
CREATE TABLE custom_skins (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    owner_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name        TEXT NOT NULL,
    type        INTEGER NOT NULL DEFAULT 0,
    weapon      INTEGER NOT NULL DEFAULT 0,
    rarity      INTEGER NOT NULL DEFAULT 0,
    item_config TEXT NOT NULL DEFAULT '{}',
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    updated_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX custom_skins_owner_idx ON custom_skins (owner_id, id);
