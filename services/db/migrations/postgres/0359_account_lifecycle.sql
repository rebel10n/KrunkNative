-- 0359_account_lifecycle (PostgreSQL): Krunker v10.0.3's account window (windows[4]) behind kn_brain
-- (services/brain/account_life.c). Additive only.
--
-- accounts.delete_requested_at: v10's player_cancelrequest (acc.reqDel, deob-10.0.3.js:128940): set by
-- rpc("rqd", 1), cleared by rpc("rqd", 0) (the client sends it at its next login, 244449). An account whose
-- request is older than accounts.deletion_grace_days (14, the "account.delete" text) and that has not logged in
-- since is deleted; every reference to accounts is ON DELETE CASCADE / SET NULL.
-- accounts.password_changed_at: the Account Security change ("You can update your password every 30 minutes").
-- players.tw_userid: v10's tw_userid (acc.twitchId, 128954), the linked Twitch user; tw_twitchname (0100) its login.
-- twitch_links: the permanent account <-> Twitch user pair ("Twitch linking creates a permanent connection to this
-- Krunker account regardless of unlinking", 247700; "Unlinking does not allow you to link a seperate twitch
-- account"): an account keeps its Twitch user and a Twitch user its account after an unlink.
-- twitch_link_states: the OAuth state of a started link (sha256 of the nonce, like sessions.token_hash), single use.
ALTER TABLE accounts ADD COLUMN delete_requested_at timestamptz;
ALTER TABLE accounts ADD COLUMN password_changed_at timestamptz;
ALTER TABLE players ADD COLUMN tw_userid text;
CREATE INDEX accounts_delete_requested_idx ON accounts (delete_requested_at) WHERE delete_requested_at IS NOT NULL;

CREATE TABLE twitch_links (
    account_id   integer PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    twitch_id    text NOT NULL UNIQUE,
    twitch_name  text NOT NULL,
    linked_at    timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE twitch_link_states (
    state_hash   bytea PRIMARY KEY,
    account_id   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at   timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX twitch_link_states_account_idx ON twitch_link_states (account_id);
