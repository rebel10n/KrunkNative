-- 0111_economy: KR gifting, the market and trading on the coordinator (doc/v7/economy.md).
-- Additive and forward-only: existing rows keep working (new columns are nullable or have a
-- constant default, which PostgreSQL adds without rewriting the table), nothing is dropped and no
-- data is rewritten, so it is safe to apply on the live database while kn_brain is serving.
-- The new foreign key on kr_transactions is NOT VALID: it checks new rows only, so adding it does
-- not scan the existing ledger.
--
--  * kr_transactions gains the counterparty of a transfer and the fee charged with it, so a row is
--    a complete record of a movement (reason = source, counterparty, reference = ref, fee).
--  * trades gains the KR each side offers (an offer is items + KR), the TTL of an open offer and
--    `escrow`: 1 when the sender's items and KR left the sender's account
--    when the offer was made (every offer made by this version), 0 for older open offers, which
--    are settled the old way at accept time.
--  * economy_requests: one row per client action key (idempotency) - a retried frame replays the
--    first result instead of spending twice. Its rows also drive the per-account rate limits.
--  * feature_bans: per-feature restrictions (gift / market / trade / ...) that staff set without
--    banning the whole account. A row with revoked_at NULL and expires_at NULL or in the future
--    is in force.
--  * kn_add_funds(id, amount, source, ref, counterparty, fee): the 6-argument overload of the
--    0103 function for SQL callers; the 4-argument kn_add_funds stays exactly as it is.
--  * trades_escrow_guard: an escrowed open offer can only be closed by a transaction that returns
--    or delivers its escrow (kn_brain's db_economy.c marks those with set_config('kn.economy',
--    '1', true)). A brain still running the previous version during a rolling deploy (its 0104
--    kn_trade_accept / kn_trade_close and "trCA" UPDATE) or a hand-written UPDATE then fails
--    instead of stranding the escrowed items and KR.

-- KR ledger: who moved the money and what the movement cost (fee >= 0, in KR, already included
-- in amount for the payer's row).
ALTER TABLE kr_transactions ADD COLUMN counterparty_id integer;
ALTER TABLE kr_transactions ADD COLUMN fee bigint NOT NULL DEFAULT 0;
ALTER TABLE kr_transactions ADD CONSTRAINT kr_transactions_counterparty_fk
    FOREIGN KEY (counterparty_id) REFERENCES accounts(id) ON DELETE SET NULL NOT VALID;

-- Offers: items + KR, with an optional TTL.
ALTER TABLE trades ADD COLUMN sender_kr   bigint NOT NULL DEFAULT 0;
ALTER TABLE trades ADD COLUMN receiver_kr bigint NOT NULL DEFAULT 0;
ALTER TABLE trades ADD COLUMN expires_at  timestamptz;
ALTER TABLE trades ADD COLUMN escrow      smallint NOT NULL DEFAULT 0;
ALTER TABLE trades ADD CONSTRAINT trades_kr_nonneg CHECK (sender_kr >= 0 AND receiver_kr >= 0) NOT VALID;
CREATE INDEX trades_open_ttl_idx ON trades (expires_at) WHERE status = 'open';

-- Idempotency + rate limits: one row per (account, action kind, client key), holding the JSON
-- result of the run that created it. The claim (INSERT .. ON CONFLICT DO NOTHING) happens inside
-- the transfer's transaction, so a concurrent retry with the same key waits for it and then
-- replays its result, and a transfer that failed and rolled back leaves no row behind.
CREATE TABLE economy_requests (
    account_id integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind       text    NOT NULL,             -- 'gift' | 'list' | 'unlist' | 'buy' | 'qsell' | 'trade' | 'accept' | 'close'
    req_key    text    NOT NULL CHECK (length(req_key) BETWEEN 1 AND 64),
    response   text,
    at         timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, kind, req_key)
);
CREATE INDEX economy_requests_at_idx ON economy_requests (account_id, at);

-- Per-feature bans ('gift' | 'market' | 'trade' | 'chat' | 'post' | 'clan' | 'publish').
CREATE TABLE feature_bans (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    feature     text NOT NULL,
    reason      text NOT NULL DEFAULT '',
    created_by  integer REFERENCES accounts(id) ON DELETE SET NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz,
    revoked_at  timestamptz
);
CREATE INDEX feature_bans_account_idx ON feature_bans (account_id, feature) WHERE revoked_at IS NULL;

-- KR delta with a full ledger row (counterparty, fee); same rules as the 4-argument version.
CREATE OR REPLACE FUNCTION kn_add_funds(p_id integer, p_amount bigint, p_source text, p_ref text,
                                        p_counterparty integer, p_fee bigint)
RETURNS bigint LANGUAGE plpgsql AS $fn$
DECLARE
    v_funds bigint;
BEGIN
    UPDATE players SET player_funds = player_funds + p_amount, updated_at = now()
    WHERE player_id = p_id AND player_funds + p_amount >= 0
    RETURNING player_funds INTO v_funds;
    IF NOT FOUND THEN
        IF EXISTS (SELECT 1 FROM players WHERE player_id = p_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Not enough KR';
        END IF;
        RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Account not found.';
    END IF;
    IF p_amount <> 0 THEN
        INSERT INTO kr_transactions (account_id, amount, source, ref, counterparty_id, fee)
        VALUES (p_id, p_amount, p_source, p_ref, nullif(p_counterparty, 0), greatest(coalesce(p_fee, 0), 0));
    END IF;
    RETURN v_funds;
END
$fn$;

-- Escrowed offers close only through the code that unwinds them (see the header).
CREATE OR REPLACE FUNCTION kn_trades_escrow_guard() RETURNS trigger LANGUAGE plpgsql AS $fn$
BEGIN
    IF OLD.escrow = 1 AND OLD.status = 'open' AND NEW.status <> 'open'
       AND coalesce(current_setting('kn.economy', true), '') <> '1' THEN
        RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Trade no longer available';
    END IF;
    RETURN NEW;
END
$fn$;
CREATE TRIGGER trades_escrow_guard BEFORE UPDATE OF status ON trades
    FOR EACH ROW EXECUTE PROCEDURE kn_trades_escrow_guard();
