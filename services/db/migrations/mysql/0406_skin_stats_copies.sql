-- 0406_skin_stats_copies (MySQL / MariaDB): same as postgres/0406. Krunker v10.0.7 counts a counter skin's stat on the copy
-- that counted it (takeSkinStats, deob-10.0.7.js:191914-191936; the base is the worn copy's m[stat] || 0, :173031).
-- skin_stats.copy_id: the copy (skin_copies.copy_id), 0 the stack, in the primary key. An existing row goes on the index's
-- first owned entry, where db_owned_skins showed it: the stack when the account has unpainted units, else the lowest copy id.
ALTER TABLE skin_stats ADD COLUMN copy_id BIGINT NOT NULL DEFAULT 0 CHECK (copy_id >= 0) AFTER skin_id;
UPDATE skin_stats s SET copy_id =
    CASE WHEN coalesce((SELECT i.count FROM inventory i WHERE i.account_id = s.account_id AND i.skin_id = s.skin_id), 0)
              > (SELECT count(*) FROM skin_copies c WHERE c.account_id = s.account_id AND c.skin_id = s.skin_id)
         THEN 0
         ELSE coalesce((SELECT min(c.copy_id) FROM skin_copies c WHERE c.account_id = s.account_id AND c.skin_id = s.skin_id), 0)
    END;
ALTER TABLE skin_stats DROP PRIMARY KEY, ADD PRIMARY KEY (account_id, skin_id, copy_id, stat);
