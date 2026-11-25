-- 0408_skin_stats_stranded (PostgreSQL): Krunker v10.0.7 keeps a counter skin's count in the copy's m[stat]
-- (takeSkinStats, deob-10.0.7.js:191932), so the count is wherever the copy is. Before 0407 a copy changed owner
-- without its skin_stats rows (0406): a sale, an offer or a live trade left them on the old owner under the old copy
-- id, and a quick sell or a craft left them on a copy id no skin_copies row has. Rows on a copy that another account
-- (or another store index) holds now go to that copy's owner, summed into a count the owner has for the same stat.
-- The sender's copy in an open offer has no skin_copies row while escrowed: its counts go into its trade_copies.stats
-- (JSON {stat: count} as text, 0407) when that is empty, so an accept or a cancel gives them to the copy again. The
-- rows of every other copy id with no skin_copies row are dropped, as copy_take drops a copy's rows today. A listed
-- copy's escrow row has no copy id, so its counts cannot be matched and are dropped with the rest. Stack rows
-- (copy_id 0) do not change.
UPDATE trade_copies tc SET stats = (SELECT json_object_agg(s.stat, s.count)::text FROM skin_stats s
                                    WHERE s.copy_id = tc.copy_id AND s.count > 0)
WHERE tc.stats IS NULL
  AND EXISTS (SELECT 1 FROM trades t WHERE t.id = tc.trade_id AND t.status = 'open')
  AND NOT EXISTS (SELECT 1 FROM skin_copies c WHERE c.copy_id = tc.copy_id)
  AND EXISTS (SELECT 1 FROM skin_stats s WHERE s.copy_id = tc.copy_id AND s.count > 0);
INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count)
    SELECT c.account_id, c.skin_id, s.copy_id, s.stat, sum(s.count)
    FROM skin_stats s JOIN skin_copies c ON c.copy_id = s.copy_id
    WHERE s.copy_id > 0 AND (s.account_id <> c.account_id OR s.skin_id <> c.skin_id)
    GROUP BY c.account_id, c.skin_id, s.copy_id, s.stat
ON CONFLICT (account_id, skin_id, copy_id, stat) DO UPDATE SET count = skin_stats.count + excluded.count;
DELETE FROM skin_stats s
WHERE s.copy_id > 0 AND NOT EXISTS (SELECT 1 FROM skin_copies c
                                    WHERE c.copy_id = s.copy_id AND c.account_id = s.account_id AND c.skin_id = s.skin_id);
