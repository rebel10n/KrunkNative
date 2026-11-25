-- 0407_copy_stats_escrow (SQLite): Krunker v10.0.7 keeps a counter skin's count in the copy's m[stat] (takeSkinStats,
-- deob-10.0.7.js:191932), so it goes with the copy: a listing's m carries it (the item view draws each stat of the
-- listing's metadata, :106682, :106774) and the buyer's copy has it. Box Office keeps counts in skin_stats per copy
-- (0406); a listed copy and the sender's copy in an open offer have no skin_copies row while escrowed, so their counts
-- wait on the escrow row as their paint does (0353): market_listings.stats and trade_copies.stats, JSON {stat: count},
-- NULL for none. A stack's unit carries none (its listing has no m). Additive only.
ALTER TABLE market_listings ADD COLUMN stats TEXT;
ALTER TABLE trade_copies ADD COLUMN stats TEXT;
