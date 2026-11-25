-- 0406_skin_stats_copies (SQLite): Krunker v10.0.7 counts a counter skin's stat on the copy that counted it: takeSkinStats
-- sends {playerId, skinId: the copy's id, stat, count} and adds the count to that copy's m[stat] (deob-10.0.7.js:191914-191936),
-- and the counter's base is the worn copy's own m[stat] || 0 (:173031, the slot's m from :174120). Box Office kept one count
-- per store index (0365), so a copy that was not the index's first entry read 0 after a reload. skin_stats.copy_id: the
-- copy (skin_copies.copy_id), 0 the stack (the unpainted units, as item_locks 0364 and item_styles 0393), and it joins the
-- primary key. An existing row goes on the entry db_owned_skins showed it on, the index's first owned entry: the stack when
-- the account has unpainted units of the index, else its lowest copy id (0 when it owns none), so no count changes entry.
CREATE TABLE skin_stats_0406 (
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     INTEGER NOT NULL CHECK (skin_id >= 0),
    copy_id     INTEGER NOT NULL DEFAULT 0 CHECK (copy_id >= 0),
    stat        TEXT NOT NULL,
    count       INTEGER NOT NULL DEFAULT 0 CHECK (count >= 0),
    PRIMARY KEY (account_id, skin_id, copy_id, stat)
);
INSERT INTO skin_stats_0406 (account_id, skin_id, copy_id, stat, count)
    SELECT s.account_id, s.skin_id,
           CASE WHEN coalesce((SELECT i.count FROM inventory i WHERE i.account_id = s.account_id AND i.skin_id = s.skin_id), 0)
                     > (SELECT count(*) FROM skin_copies c WHERE c.account_id = s.account_id AND c.skin_id = s.skin_id)
                THEN 0
                ELSE coalesce((SELECT min(c.copy_id) FROM skin_copies c WHERE c.account_id = s.account_id AND c.skin_id = s.skin_id), 0)
           END,
           s.stat, s.count
    FROM skin_stats s;
DROP TABLE skin_stats;
ALTER TABLE skin_stats_0406 RENAME TO skin_stats;
