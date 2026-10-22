-- 0353_paint_trade (SQLite): a painted copy (0352) can be listed, offered in a trade and sold, and keeps its paint
-- there (doc/v10/paint-copy.md). market_listings.paint_* is the paint of a listed copy (NULL for a stack's unit), so
-- a sale, a buy or an unlist gives the copy back with its paint. trade_copies holds the copies of an offer: side 's'
-- the sender's (escrowed: their skin_copies row is gone while the offer is open, and paint_* keeps the paint), side
-- 'r' the receiver's (still theirs until the offer is accepted). trade_items keeps the unpainted units only. Additive.
ALTER TABLE market_listings ADD COLUMN paint_color TEXT;
ALTER TABLE market_listings ADD COLUMN paint_shader INTEGER;
CREATE TABLE trade_copies (
    trade_id     INTEGER NOT NULL REFERENCES trades(id) ON DELETE CASCADE,
    side         TEXT NOT NULL CHECK (side IN ('s', 'r')),
    copy_id      INTEGER NOT NULL,
    skin_id      INTEGER NOT NULL,
    paint_color  TEXT,
    paint_shader INTEGER,
    PRIMARY KEY (trade_id, side, copy_id)
);
