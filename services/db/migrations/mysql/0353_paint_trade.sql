-- 0353_paint_trade (MySQL/MariaDB): same as postgres/0353: a painted copy (0352) can be listed, offered in a trade
-- and sold, and keeps its paint there (doc/v10/paint-copy.md). market_listings.paint_* is the paint of a listed
-- copy; trade_copies holds an offer's copies ('s' escrowed, 'r' the receiver's). Additive.
ALTER TABLE market_listings ADD COLUMN paint_color VARCHAR(16) NULL, ADD COLUMN paint_shader INT NULL;
CREATE TABLE trade_copies (
    trade_id     BIGINT NOT NULL REFERENCES trades(id) ON DELETE CASCADE,
    side         CHAR(1) NOT NULL CHECK (side IN ('s', 'r')),
    copy_id      BIGINT NOT NULL,
    skin_id      INT NOT NULL,
    paint_color  VARCHAR(16) NULL,
    paint_shader INT NULL,
    PRIMARY KEY (trade_id, side, copy_id)
);
