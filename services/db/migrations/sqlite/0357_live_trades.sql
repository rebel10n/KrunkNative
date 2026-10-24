-- 0357_live_trades (SQLite): same as postgres/0357: the live trades of Trade mode (doc/v10/trade.md), one row per
-- settled trade (req_key = the game server's session key, unique: a resent settle never moves anything twice) and
-- one live_trade_items row per stack / copy moved ('a' given by account_a, 'b' by account_b). Additive.
CREATE TABLE live_trades (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    req_key     TEXT NOT NULL UNIQUE,
    account_a   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    account_b   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kr_a        INTEGER NOT NULL DEFAULT 0 CHECK (kr_a >= 0),
    kr_b        INTEGER NOT NULL DEFAULT 0 CHECK (kr_b >= 0),
    fee         INTEGER NOT NULL DEFAULT 0 CHECK (fee >= 0),
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    CHECK (account_a <> account_b)
);
CREATE INDEX live_trades_a_idx ON live_trades (account_a, created_at);
CREATE INDEX live_trades_b_idx ON live_trades (account_b, created_at);
CREATE TABLE live_trade_items (
    trade_id     INTEGER NOT NULL REFERENCES live_trades(id) ON DELETE CASCADE,
    side         TEXT NOT NULL CHECK (side IN ('a', 'b')),
    skin_id      INTEGER NOT NULL,
    count        INTEGER NOT NULL CHECK (count > 0),
    copy_id      INTEGER,
    new_copy_id  INTEGER
);
CREATE INDEX live_trade_items_trade_idx ON live_trade_items (trade_id);
