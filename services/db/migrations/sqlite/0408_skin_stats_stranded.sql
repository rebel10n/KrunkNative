-- 0408_skin_stats_stranded (SQLite): same as postgres/0408. Krunker v10.0.7 keeps a counter skin's count in the copy's
-- m[stat] (deob-10.0.7.js:191932), so the count is wherever the copy is. Before 0407 a copy changed owner without its
-- skin_stats rows (0406). Rows on a copy another account (or store index) holds now go to that copy's owner, summed into
-- a count the owner has for the same stat; an open offer's escrowed copy (no skin_copies row) gets them in its
-- trade_copies.stats when that is empty; the rows of every other copy id with no skin_copies row are dropped, as
-- copy_take drops a copy's rows today. Stack rows (copy_id 0) do not change.
UPDATE trade_copies SET stats = (SELECT json_group_object(s.stat, s.count) FROM skin_stats s
                                 WHERE s.copy_id = trade_copies.copy_id AND s.count > 0)
WHERE stats IS NULL
  AND EXISTS (SELECT 1 FROM trades t WHERE t.id = trade_copies.trade_id AND t.status = 'open')
  AND NOT EXISTS (SELECT 1 FROM skin_copies c WHERE c.copy_id = trade_copies.copy_id)
  AND EXISTS (SELECT 1 FROM skin_stats s WHERE s.copy_id = trade_copies.copy_id AND s.count > 0);
INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count)
    SELECT c.account_id, c.skin_id, s.copy_id, s.stat, sum(s.count)
    FROM skin_stats s JOIN skin_copies c ON c.copy_id = s.copy_id
    WHERE s.copy_id > 0 AND (s.account_id <> c.account_id OR s.skin_id <> c.skin_id)
    GROUP BY c.account_id, c.skin_id, s.copy_id, s.stat
ON CONFLICT (account_id, skin_id, copy_id, stat) DO UPDATE SET count = skin_stats.count + excluded.count;
DELETE FROM skin_stats
WHERE copy_id > 0 AND NOT EXISTS (SELECT 1 FROM skin_copies c WHERE c.copy_id = skin_stats.copy_id
                                  AND c.account_id = skin_stats.account_id AND c.skin_id = skin_stats.skin_id);
