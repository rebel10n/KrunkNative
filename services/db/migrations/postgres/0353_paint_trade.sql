-- 0353_paint_trade (PostgreSQL): a painted copy (0352) can be listed, offered in a trade and sold, and keeps its
-- paint there (doc/v10/paint-copy.md). market_listings.paint_* is the paint of a listed copy (NULL for a stack's
-- unit); trade_copies holds an offer's copies: side 's' the sender's (escrowed, paint kept), side 'r' the receiver's
-- (still theirs until accepted). trade_items keeps the unpainted units only. Additive.
ALTER TABLE market_listings ADD COLUMN paint_color text, ADD COLUMN paint_shader integer;
CREATE TABLE trade_copies (
    trade_id     bigint NOT NULL REFERENCES trades(id) ON DELETE CASCADE,
    side         text NOT NULL CHECK (side IN ('s', 'r')),
    copy_id      bigint NOT NULL,
    skin_id      integer NOT NULL,
    paint_color  text,
    paint_shader integer,
    PRIMARY KEY (trade_id, side, copy_id)
);
