-- 0357_live_trades (MySQL/MariaDB): same as postgres/0357: the live trades of Trade mode (doc/v10/trade.md), one row
-- per settled trade (req_key = the game server's session key, unique: a resent settle never moves anything twice)
-- and one live_trade_items row per stack / copy moved ('a' given by account_a, 'b' by account_b). Additive.
CREATE TABLE live_trades (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    req_key     VARCHAR(64) NOT NULL UNIQUE,
    account_a   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    account_b   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kr_a        BIGINT NOT NULL DEFAULT 0 CHECK (kr_a >= 0),
    kr_b        BIGINT NOT NULL DEFAULT 0 CHECK (kr_b >= 0),
    fee         BIGINT NOT NULL DEFAULT 0 CHECK (fee >= 0),
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    CHECK (account_a <> account_b)
);
CREATE INDEX live_trades_a_idx ON live_trades (account_a, created_at);
CREATE INDEX live_trades_b_idx ON live_trades (account_b, created_at);
CREATE TABLE live_trade_items (
    trade_id     BIGINT NOT NULL REFERENCES live_trades(id) ON DELETE CASCADE,
    side         CHAR(1) NOT NULL CHECK (side IN ('a', 'b')),
    skin_id      INT NOT NULL,
    count        INT NOT NULL CHECK (count > 0),
    copy_id      BIGINT NULL,
    new_copy_id  BIGINT NULL
);
CREATE INDEX live_trade_items_trade_idx ON live_trade_items (trade_id);
