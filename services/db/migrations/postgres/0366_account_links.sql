-- 0366_account_links (PostgreSQL): Krunker v10.0.7's account links behind kn_brain. Additive only.
--
-- Twitch (services/brain/account_life.c): the link keeps its OAuth tokens so drops can tell an expired
-- link (v10.0.7 dropsLoaded(drops, reconnect), deob-10.0.7.js:218880-218924). token_enc / refresh_enc are
-- knsvc kn_seal values (AES-256-GCM, accounts.secrets_key), never plaintext. token_state: 'ok' while the
-- token validates or refreshes, 'expired' once Twitch refuses both (the reconnect flag), NULL for a link
-- made before the tokens were kept (also asked to reconnect).
--
-- Discord (services/brain/discord.c): the v10 Discord link (discord.getStatus / link / joinGuild / unlink,
-- deob-10.0.7.js:26499-26523; the component :97250-97480). One Discord user per account and per Discord
-- user; discord_link_states is the OAuth state of a started link (sha256 of the nonce, single use).
--
-- Login with Box Office (services/brain/oauth.c, doc/v10/oauth-provider.md): v10.0.7's "Login with
-- Krunker" (the oauth API :27656-27696, Settings > Connected apps :97493-97673) with kn_brain as the
-- provider for Box Office accounts. oauth_clients are the registered apps (secret_hash NULL = a public
-- client, PKCE only); oauth_codes the single-use authorization codes; oauth_grants the connected apps
-- (one per account and app: what Settings lists); oauth_tokens the access / refresh tokens (sha256);
-- oauth_actions the write requests an app sends for the player to approve with 2FA (oauth.action.*);
-- oauth_activity the per-app log (settings.apps.activity).
ALTER TABLE twitch_links ADD COLUMN token_enc text;
ALTER TABLE twitch_links ADD COLUMN refresh_enc text;
ALTER TABLE twitch_links ADD COLUMN token_expires_at timestamptz;
ALTER TABLE twitch_links ADD COLUMN token_checked_at timestamptz;
ALTER TABLE twitch_links ADD COLUMN token_state text;

CREATE TABLE discord_links (
    account_id   integer PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    discord_id   text NOT NULL UNIQUE,
    username     text NOT NULL,
    global_name  text,
    avatar       text,
    token_enc    text,
    refresh_enc  text,
    token_expires_at timestamptz,
    in_guild     boolean NOT NULL DEFAULT false,
    linked_at    timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE discord_link_states (
    state_hash   bytea PRIMARY KEY,
    account_id   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    return_to    text,
    created_at   timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX discord_link_states_account_idx ON discord_link_states (account_id);

CREATE TABLE oauth_clients (
    client_id     text PRIMARY KEY,
    secret_hash   bytea,
    name          text NOT NULL,
    developer     text,
    icon_url      text,
    homepage_url  text,
    privacy_url   text,
    tos_url       text,
    redirect_uris text NOT NULL,
    scopes        text NOT NULL,
    min_level     integer NOT NULL DEFAULT 0,
    owner_id      integer REFERENCES accounts(id) ON DELETE SET NULL,
    disabled      boolean NOT NULL DEFAULT false,
    created_at    timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE oauth_codes (
    code_hash      bytea PRIMARY KEY,
    client_id      text NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    account_id     integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    redirect_uri   text NOT NULL,
    scopes         text NOT NULL,
    code_challenge text NOT NULL,
    created_at     timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE oauth_grants (
    account_id    integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    client_id     text NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    scopes        text NOT NULL,
    created_at    timestamptz NOT NULL DEFAULT now(),
    last_used_at  timestamptz,
    PRIMARY KEY (account_id, client_id)
);

CREATE TABLE oauth_tokens (
    token_hash    bytea PRIMARY KEY,
    kind          text NOT NULL,
    client_id     text NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    account_id    integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    scopes        text NOT NULL,
    expires_at    timestamptz NOT NULL,
    created_at    timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX oauth_tokens_grant_idx ON oauth_tokens (account_id, client_id);

CREATE TABLE oauth_actions (
    action_id     text PRIMARY KEY,
    client_id     text NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    account_id    integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind          text NOT NULL,
    scope         text NOT NULL,
    params        text NOT NULL,
    status        text NOT NULL DEFAULT 'pending',
    result        text,
    created_at    timestamptz NOT NULL DEFAULT now(),
    decided_at    timestamptz
);
CREATE INDEX oauth_actions_account_idx ON oauth_actions (account_id, created_at);

CREATE TABLE oauth_activity (
    id            bigserial PRIMARY KEY,
    account_id    integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    client_id     text NOT NULL REFERENCES oauth_clients(client_id) ON DELETE CASCADE,
    action        text NOT NULL,
    scope         text,
    result        text NOT NULL,
    created_at    timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX oauth_activity_grant_idx ON oauth_activity (account_id, client_id, created_at);
