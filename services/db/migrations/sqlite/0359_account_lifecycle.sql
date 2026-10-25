-- 0359_account_lifecycle (SQLite): same as postgres/0359. Krunker v10.0.3's account window behind kn_brain
-- (services/brain/account_life.c): the deletion request (v10 player_cancelrequest), the password change clock,
-- the Twitch link (tw_userid, the permanent twitch_links pair, the OAuth states). Additive only.
ALTER TABLE accounts ADD COLUMN delete_requested_at TIMESTAMP;
ALTER TABLE accounts ADD COLUMN password_changed_at TIMESTAMP;
ALTER TABLE players ADD COLUMN tw_userid TEXT;
CREATE INDEX accounts_delete_requested_idx ON accounts (delete_requested_at) WHERE delete_requested_at IS NOT NULL;

CREATE TABLE twitch_links (
    account_id   INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    twitch_id    TEXT NOT NULL UNIQUE,
    twitch_name  TEXT NOT NULL,
    linked_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);

CREATE TABLE twitch_link_states (
    state_hash   TEXT PRIMARY KEY,
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX twitch_link_states_account_idx ON twitch_link_states (account_id);
