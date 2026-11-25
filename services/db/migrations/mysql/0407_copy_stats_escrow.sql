-- 0407_copy_stats_escrow (MySQL / MariaDB): same as postgres/0407. A counter skin's count is the copy's m[stat] in
-- Krunker v10.0.7 (deob-10.0.7.js:191932) and goes with the copy; a listed or offered copy's counts wait on the escrow
-- row as its paint does: market_listings.stats and trade_copies.stats, JSON {stat: count}, NULL for none. Additive only.
ALTER TABLE market_listings ADD COLUMN stats TEXT NULL;
ALTER TABLE trade_copies ADD COLUMN stats TEXT NULL;
