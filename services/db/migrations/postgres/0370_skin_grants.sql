-- 0370_skin_grants (PostgreSQL): Box Office raid / found-skin grants made idempotent (Krunker v10.0.7: "Fixed double raid
-- drops sometimes being lost", a krunker.io server fix with no game.js hunk). A game server names every give_skin it
-- sends (grant_ref, unique per server process and grant) and posts it reliably, so a frame lost with the brain link is
-- resent; the brain grants a ref once (services/brain/db_accounts.c run_give_skin_ref). Additive only.
CREATE TABLE skin_grants (
    grant_ref   text PRIMARY KEY,
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     integer NOT NULL,
    granted_at  timestamptz NOT NULL DEFAULT now()
);
