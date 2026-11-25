-- 0408_skin_stats_stranded (MySQL / MariaDB): same as postgres/0408. Krunker v10.0.7 keeps a counter skin's count in
-- the copy's m[stat] (deob-10.0.7.js:191932), so the count is wherever the copy is. Before 0407 a copy changed owner
-- without its skin_stats rows (0406). A count moves only where it provably belongs, and nothing is deleted that was
-- not written elsewhere first: a copy another account (or store index) holds goes to that owner, summed into a count
-- it has; a copy with no skin_copies row that the same account offers in one open trade (side 's', empty stats) goes
-- into that trade_copies.stats; else, with exactly one such copy id of the index and exactly one open copy listing of
-- the index with empty stats, into that listing's stats. Every other row stays: nothing records a quick-sold or
-- crafted copy's id, and a stranded row is never read. Stack rows (copy_id 0) do not change. Reads of a table the
-- same statement writes go through a grouped derived table, which MySQL materializes before the write (error 1093
-- otherwise).
CREATE TABLE skin_stats_0408 (
    account_id INT NOT NULL,
    skin_id    INT NOT NULL,
    copy_id    BIGINT NOT NULL,
    trade_id   BIGINT NULL,
    listing_id BIGINT NULL
);
INSERT INTO skin_stats_0408 (account_id, skin_id, copy_id, trade_id)
    SELECT g.account_id, g.skin_id, g.copy_id, min(tc.trade_id)
    FROM (SELECT DISTINCT s.account_id, s.skin_id, s.copy_id FROM skin_stats s
          WHERE s.copy_id > 0 AND NOT EXISTS (SELECT 1 FROM skin_copies c WHERE c.copy_id = s.copy_id)) g
    JOIN trade_copies tc ON tc.copy_id = g.copy_id AND tc.skin_id = g.skin_id AND tc.side = 's' AND tc.stats IS NULL
    JOIN trades t ON t.id = tc.trade_id AND t.status = 'open' AND t.sender_id = g.account_id
    GROUP BY g.account_id, g.skin_id, g.copy_id
    HAVING count(*) = 1;
INSERT INTO skin_stats_0408 (account_id, skin_id, copy_id, listing_id)
    SELECT g.account_id, g.skin_id, min(g.copy_id), min(l.id)
    FROM (SELECT DISTINCT s.account_id, s.skin_id, s.copy_id FROM skin_stats s
          WHERE s.copy_id > 0 AND NOT EXISTS (SELECT 1 FROM skin_copies c WHERE c.copy_id = s.copy_id)
            AND s.copy_id NOT IN (SELECT q.copy_id FROM (SELECT copy_id FROM skin_stats_0408 GROUP BY copy_id) q)) g
    JOIN market_listings l ON l.seller_id = g.account_id AND l.skin_id = g.skin_id
        AND l.sold_at IS NULL AND l.cancelled_at IS NULL AND l.stats IS NULL
        AND (l.paint_color IS NOT NULL OR l.paint_shader IS NOT NULL OR l.wear IS NOT NULL OR l.serial IS NOT NULL
             OR l.name_tag IS NOT NULL)
    GROUP BY g.account_id, g.skin_id
    HAVING count(DISTINCT g.copy_id) = 1 AND count(DISTINCT l.id) = 1;
UPDATE trade_copies tc SET stats = (SELECT JSON_OBJECTAGG(s.stat, s.count) FROM skin_stats s
                                    JOIN skin_stats_0408 p ON p.account_id = s.account_id AND p.skin_id = s.skin_id
                                                          AND p.copy_id = s.copy_id
                                    WHERE p.trade_id = tc.trade_id AND p.copy_id = tc.copy_id AND s.count > 0)
WHERE tc.side = 's' AND EXISTS (SELECT 1 FROM skin_stats_0408 p WHERE p.trade_id = tc.trade_id AND p.copy_id = tc.copy_id);
UPDATE market_listings l SET stats = (SELECT JSON_OBJECTAGG(s.stat, s.count) FROM skin_stats s
                                      JOIN skin_stats_0408 p ON p.account_id = s.account_id AND p.skin_id = s.skin_id
                                                            AND p.copy_id = s.copy_id
                                      WHERE p.listing_id = l.id AND s.count > 0)
WHERE EXISTS (SELECT 1 FROM skin_stats_0408 p WHERE p.listing_id = l.id);
DELETE FROM skin_stats
WHERE EXISTS (SELECT 1 FROM skin_stats_0408 p WHERE p.account_id = skin_stats.account_id
              AND p.skin_id = skin_stats.skin_id AND p.copy_id = skin_stats.copy_id);
INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count)
    SELECT m.account_id, m.skin_id, m.copy_id, m.stat, m.n FROM (
        SELECT c.account_id, c.skin_id, s.copy_id, s.stat, sum(s.count) AS n
        FROM skin_stats s JOIN skin_copies c ON c.copy_id = s.copy_id
        WHERE s.copy_id > 0 AND (s.account_id <> c.account_id OR s.skin_id <> c.skin_id)
        GROUP BY c.account_id, c.skin_id, s.copy_id, s.stat) m
ON DUPLICATE KEY UPDATE count = skin_stats.count + VALUES(count);
DELETE FROM skin_stats
WHERE copy_id > 0 AND EXISTS (SELECT 1 FROM skin_copies c WHERE c.copy_id = skin_stats.copy_id
                              AND (c.account_id <> skin_stats.account_id OR c.skin_id <> skin_stats.skin_id));
DROP TABLE skin_stats_0408;
