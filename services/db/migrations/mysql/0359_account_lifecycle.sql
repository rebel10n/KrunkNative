-- 0359_account_lifecycle (MySQL / MariaDB): same as postgres/0359. Krunker v10.0.3's account window behind
-- kn_brain (services/brain/account_life.c): the deletion request (v10 player_cancelrequest), the password change
-- clock, the Twitch link (tw_userid, the permanent twitch_links pair, the OAuth states). Additive only.
ALTER TABLE accounts ADD COLUMN delete_requested_at DATETIME(3) NULL;
ALTER TABLE accounts ADD COLUMN password_changed_at DATETIME(3) NULL;
ALTER TABLE players ADD COLUMN tw_userid VARCHAR(64) NULL;
CREATE INDEX accounts_delete_requested_idx ON accounts (delete_requested_at);

CREATE TABLE twitch_links (
    account_id   BIGINT PRIMARY KEY,
    twitch_id    VARCHAR(64) NOT NULL UNIQUE,
    twitch_name  VARCHAR(64) NOT NULL,
    linked_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);

CREATE TABLE twitch_link_states (
    state_hash   VARCHAR(64) PRIMARY KEY,
    account_id   BIGINT NOT NULL,
    created_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX twitch_link_states_account_idx ON twitch_link_states (account_id);
