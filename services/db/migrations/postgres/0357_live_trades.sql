-- 0357_live_trades (PostgreSQL): the live trades of Trade mode (doc/v10/trade.md), settled by the brain's live_trade
-- op (db_economy.c run_live_trade) in one transaction: both sides' units, KR and the tradeCost of each side. One row
-- per settled trade; req_key is the game server's session key ("lt:<hex>"), unique, so a resent settle can never move
-- anything twice (economy_requests replays its result first). live_trade_items: one row per stack / copy moved, side
-- 'a' given by account_a, 'b' by account_b; a copy keeps its old and its new copy id. Additive.
CREATE TABLE live_trades (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    req_key     text NOT NULL UNIQUE,
    account_a   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    account_b   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kr_a        bigint NOT NULL DEFAULT 0 CHECK (kr_a >= 0),
    kr_b        bigint NOT NULL DEFAULT 0 CHECK (kr_b >= 0),
    fee         bigint NOT NULL DEFAULT 0 CHECK (fee >= 0),
    created_at  timestamptz NOT NULL DEFAULT now(),
    CHECK (account_a <> account_b)
);
CREATE INDEX live_trades_a_idx ON live_trades (account_a, created_at);
CREATE INDEX live_trades_b_idx ON live_trades (account_b, created_at);
CREATE TABLE live_trade_items (
    trade_id     bigint NOT NULL REFERENCES live_trades(id) ON DELETE CASCADE,
    side         char(1) NOT NULL CHECK (side IN ('a', 'b')),
    skin_id      integer NOT NULL,
    count        integer NOT NULL CHECK (count > 0),
    copy_id      bigint,
    new_copy_id  bigint
);
CREATE INDEX live_trade_items_trade_idx ON live_trade_items (trade_id);
