-- 0358_studio_charges (PostgreSQL): the KR that monetized studios' games charge their players (v10.0.3 GAME.PAYMENTS
-- authorizeSpend / charge / transact after the player's consent, the "chreq" / "chrsp" exchange; the game server asks
-- with the "studio_charge" op, services/brain/db_studios.c P_STUDIO_CHARGE). One row per charge: the gross KR the player
-- paid (its kr_transactions row: source 'studio-charge', ref = req_key) and the studio's share, 90 % rounded down
-- (v10.0.3 studios.analytics.krHint "Studios keep 90% of the KR players spend in their games"), the Analytics tab's
-- KR Earned and Gross spent. req_key is the game server's request key, unique: a resent charge never takes the KR
-- twice. The share is booked here only: KrunkNative pays no studio out. Additive.
CREATE TABLE studio_charges (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    req_key     text NOT NULL UNIQUE,
    studio_id   integer NOT NULL REFERENCES studios(id) ON DELETE CASCADE,
    map_id      integer NOT NULL,                          -- published_maps.map_id at the charge (no FK: the row outlives the map)
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    amount      bigint NOT NULL CHECK (amount > 0),        -- the KR the player paid
    earned      bigint NOT NULL CHECK (earned >= 0),       -- the studio's share
    created_at  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX studio_charges_studio_idx ON studio_charges (studio_id, created_at);
CREATE INDEX studio_charges_account_idx ON studio_charges (account_id, created_at);
