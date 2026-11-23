-- 0395_skin_assets (SQLite): same as postgres/0395_skin_assets (see there).
-- AUTOINCREMENT: the next id is sqlite_sequence's seq + 1 (the row exists once the table had an insert)
INSERT INTO sqlite_sequence (name, seq)
    SELECT 'custom_skins', 999999999 WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = 'custom_skins');
UPDATE sqlite_sequence SET seq = max(seq, 999999999) WHERE name = 'custom_skins';
CREATE TABLE studio_skins (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    studio_id      INTEGER NOT NULL REFERENCES studios(id) ON DELETE CASCADE,
    custom_skin_id INTEGER NOT NULL,
    name           TEXT NOT NULL,
    type           INTEGER NOT NULL DEFAULT 0,
    weapon         INTEGER NOT NULL DEFAULT 0,
    rarity         INTEGER NOT NULL DEFAULT 0,
    item_config    TEXT NOT NULL DEFAULT '{}',
    active         INTEGER NOT NULL DEFAULT 0,
    created_by     INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    created_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE UNIQUE INDEX studio_skins_studio_skin_uq ON studio_skins (studio_id, custom_skin_id);
INSERT INTO sqlite_sequence (name, seq) VALUES ('studio_skins', 10000000);
