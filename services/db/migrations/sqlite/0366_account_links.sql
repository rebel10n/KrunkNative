-- 0366_account_links (SQLite): same as postgres/0366. Krunker v10.0.7's account links behind kn_brain: the
-- Twitch link's sealed tokens (account_life.c), the Discord link (discord.c), Login with Box Office (oauth.c,
-- doc/v10/oauth-provider.md). Additive only.
ALTER TABLE twitch_links ADD COLUMN token_enc TEXT;
ALTER TABLE twitch_links ADD COLUMN refresh_enc TEXT;
ALTER TABLE twitch_links ADD COLUMN token_expires_at TIMESTAMP;
ALTER TABLE twitch_links ADD COLUMN token_checked_at TIMESTAMP;
ALTER TABLE twitch_links ADD COLUMN token_state TEXT;

CREATE TABLE discord_links (
    account_id   INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    discord_id   TEXT NOT NULL UNIQUE,
    username     TEXT NOT NULL,
    global_name  TEXT,
    avatar       TEXT,
    token_enc    TEXT,
    refresh_enc  TEXT,
    token_expires_at TIMESTAMP,
    in_guild     INTEGER NOT NULL DEFAULT 0,
    linked_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);

CREATE TABLE discord_link_states (
    state_hash   TEXT PRIMARY KEY,
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    return_to    TEXT,
    created_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX discord_link_states_account_idx ON discord_link_states (account_id);

CREATE TABLE oauth_clients (
    client_id     TEXT PRIMARY KEY,
    secret_hash   TEXT,
    name          TEXT NOT NULL,
    developer     TEXT,
    icon_url      TEXT,
    homepage_url  TEXT,
    privacy_url   TEXT,
    tos_url       TEXT,
    redirect_uris TEXT NOT NULL,
    scopes        TEXT NOT NULL,
    min_level     INTEGER NOT NULL DEFAULT 0,
    owner_id      INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    disabled      INTEGER NOT NULL DEFAULT 0,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);

CREATE TABLE oauth_codes (
    code_hash      TEXT PRIMARY KEY,
    client_id      TEXT NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    account_id     INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    redirect_uri   TEXT NOT NULL,
    scopes         TEXT NOT NULL,
    code_challenge TEXT NOT NULL,
    created_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);

CREATE TABLE oauth_grants (
    account_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    client_id     TEXT NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    scopes        TEXT NOT NULL,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    last_used_at  TIMESTAMP,
    PRIMARY KEY (account_id, client_id)
);

CREATE TABLE oauth_tokens (
    token_hash    TEXT PRIMARY KEY,
    kind          TEXT NOT NULL,
    client_id     TEXT NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    account_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    scopes        TEXT NOT NULL,
    expires_at    TIMESTAMP NOT NULL,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX oauth_tokens_grant_idx ON oauth_tokens (account_id, client_id);

CREATE TABLE oauth_actions (
    action_id     TEXT PRIMARY KEY,
    client_id     TEXT NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    account_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind          TEXT NOT NULL,
    scope         TEXT NOT NULL,
    params        TEXT NOT NULL,
    status        TEXT NOT NULL DEFAULT 'pending',
    result        TEXT,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    decided_at    TIMESTAMP
);
CREATE INDEX oauth_actions_account_idx ON oauth_actions (account_id, created_at);

CREATE TABLE oauth_activity (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    client_id     TEXT NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    action        TEXT NOT NULL,
    scope         TEXT,
    result        TEXT NOT NULL,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX oauth_activity_grant_idx ON oauth_activity (account_id, client_id, created_at);
