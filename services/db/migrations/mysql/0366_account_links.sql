-- 0366_account_links (MySQL / MariaDB): same as postgres/0366. Krunker v10.0.7's account links behind kn_brain:
-- the Twitch link's sealed tokens (account_life.c), the Discord link (discord.c), Login with Box Office (oauth.c,
-- doc/v10/oauth-provider.md). Additive only.
ALTER TABLE twitch_links ADD COLUMN token_enc TEXT NULL;
ALTER TABLE twitch_links ADD COLUMN refresh_enc TEXT NULL;
ALTER TABLE twitch_links ADD COLUMN token_expires_at DATETIME(3) NULL;
ALTER TABLE twitch_links ADD COLUMN token_checked_at DATETIME(3) NULL;
ALTER TABLE twitch_links ADD COLUMN token_state VARCHAR(16) NULL;

CREATE TABLE discord_links (
    account_id   BIGINT PRIMARY KEY,
    discord_id   VARCHAR(64) NOT NULL UNIQUE,
    username     VARCHAR(64) NOT NULL,
    global_name  VARCHAR(64) NULL,
    avatar       VARCHAR(128) NULL,
    token_enc    TEXT NULL,
    refresh_enc  TEXT NULL,
    token_expires_at DATETIME(3) NULL,
    in_guild     TINYINT(1) NOT NULL DEFAULT 0,
    linked_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);

CREATE TABLE discord_link_states (
    state_hash   VARCHAR(64) PRIMARY KEY,
    account_id   BIGINT NOT NULL,
    return_to    VARCHAR(512) NULL,
    created_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX discord_link_states_account_idx ON discord_link_states (account_id);

CREATE TABLE oauth_clients (
    client_id     VARCHAR(64) PRIMARY KEY,
    secret_hash   VARCHAR(64) NULL,
    name          VARCHAR(64) NOT NULL,
    developer     VARCHAR(64) NULL,
    icon_url      VARCHAR(512) NULL,
    homepage_url  VARCHAR(512) NULL,
    privacy_url   VARCHAR(512) NULL,
    tos_url       VARCHAR(512) NULL,
    redirect_uris TEXT NOT NULL,
    scopes        TEXT NOT NULL,
    min_level     INT NOT NULL DEFAULT 0,
    owner_id      BIGINT NULL,
    disabled      TINYINT(1) NOT NULL DEFAULT 0,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (owner_id) REFERENCES accounts(id) ON DELETE SET NULL
);

CREATE TABLE oauth_codes (
    code_hash      VARCHAR(64) PRIMARY KEY,
    client_id      VARCHAR(64) NOT NULL,
    account_id     BIGINT NOT NULL,
    redirect_uri   VARCHAR(512) NOT NULL,
    scopes         TEXT NOT NULL,
    code_challenge VARCHAR(128) NOT NULL,
    created_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (client_id) REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);

CREATE TABLE oauth_grants (
    account_id    BIGINT NOT NULL,
    client_id     VARCHAR(64) NOT NULL,
    scopes        TEXT NOT NULL,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    last_used_at  DATETIME(3) NULL,
    PRIMARY KEY (account_id, client_id),
    FOREIGN KEY (client_id) REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);

CREATE TABLE oauth_tokens (
    token_hash    VARCHAR(64) PRIMARY KEY,
    kind          VARCHAR(16) NOT NULL,
    client_id     VARCHAR(64) NOT NULL,
    account_id    BIGINT NOT NULL,
    scopes        TEXT NOT NULL,
    expires_at    DATETIME(3) NOT NULL,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (client_id) REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX oauth_tokens_grant_idx ON oauth_tokens (account_id, client_id);

CREATE TABLE oauth_actions (
    action_id     VARCHAR(64) PRIMARY KEY,
    client_id     VARCHAR(64) NOT NULL,
    account_id    BIGINT NOT NULL,
    kind          VARCHAR(32) NOT NULL,
    scope         VARCHAR(64) NOT NULL,
    params        TEXT NOT NULL,
    status        VARCHAR(16) NOT NULL DEFAULT 'pending',
    result        TEXT NULL,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    decided_at    DATETIME(3) NULL,
    FOREIGN KEY (client_id) REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX oauth_actions_account_idx ON oauth_actions (account_id, created_at);

CREATE TABLE oauth_activity (
    id            BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id    BIGINT NOT NULL,
    client_id     VARCHAR(64) NOT NULL,
    action        VARCHAR(64) NOT NULL,
    scope         VARCHAR(64) NULL,
    result        VARCHAR(64) NOT NULL,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (client_id) REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX oauth_activity_grant_idx ON oauth_activity (account_id, client_id, created_at);
