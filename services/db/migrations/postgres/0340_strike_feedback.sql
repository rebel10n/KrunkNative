-- 0340_strike_feedback: Krunker Strike's feedback form (the main menu's "Help us improve" banner,
-- FRVR's POST /v1/feedback {text, email}). One row per submission by a signed-in account, kept for
-- the developers (no read route yet). Owner: services/brain/db_strike.c (P_STRIKE_FEEDBACK), route
-- /strike/v1/feedback in services/brain/strike_econ.c. Additive only.
CREATE TABLE strike_feedback (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    message     text NOT NULL,
    email       text NOT NULL DEFAULT '',          -- optional contact ("" when none)
    created_at  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX strike_feedback_account_idx ON strike_feedback (account_id, created_at DESC);
