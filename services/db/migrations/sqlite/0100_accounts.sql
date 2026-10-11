-- 0100_accounts (SQLite): credentials, profiles (Krunker v3.1.1 qs_player),
-- sessions, login audit / brute-force lockout, bans, alias history, match
-- results, KR ledger. Same tables and columns as postgres/0100 (+ 0105, 0106);
-- the multi-statement operations (postgres/0103 functions) are C in
-- services/brain/db_accounts.c. Conventions of the SQLite dialect:
--   timestamps  TIMESTAMP text 'YYYY-MM-DD HH:MM:SS.SSS' in UTC ({now} macro)
--   json        "JSON TEXT" (typed as JSON by the driver)
--   booleans    BOOLEAN 0/1
--   ids         INTEGER PRIMARY KEY AUTOINCREMENT (never reused)
--   sessions    token_hash = hex sha256 of the token ({hash:x} macro)

CREATE TABLE accounts (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    username       TEXT NOT NULL CHECK (length(username) BETWEEN 1 AND 32),
    password_hash  TEXT NOT NULL,
    email          TEXT,
    birthday       TEXT,
    referral       TEXT,
    created_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    created_ip     TEXT,
    last_login_at  TIMESTAMP,
    last_login_ip  TEXT,
    twofa_secret   TEXT,
    settings       TEXT,
    settings_at    TIMESTAMP
);
CREATE UNIQUE INDEX accounts_username_lower_uq ON accounts (lower(username));

CREATE TABLE players (
    player_id                 INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    player_alias              TEXT,
    player_kills              INTEGER NOT NULL DEFAULT 0,
    player_deaths             INTEGER NOT NULL DEFAULT 0,
    player_wins               INTEGER NOT NULL DEFAULT 0,
    player_games_played       INTEGER NOT NULL DEFAULT 0,
    player_score              INTEGER NOT NULL DEFAULT 0,
    player_timeplayed         INTEGER NOT NULL DEFAULT 0,
    player_funds              INTEGER NOT NULL DEFAULT 0 CHECK (player_funds >= 0),
    player_stats              JSON TEXT NOT NULL DEFAULT '{}',
    player_lastreward         INTEGER NOT NULL DEFAULT 0,
    player_lastfreespin       INTEGER NOT NULL DEFAULT 0,
    player_elo                INTEGER NOT NULL DEFAULT 0,
    player_elo2               INTEGER NOT NULL DEFAULT 0,
    player_elo4               INTEGER NOT NULL DEFAULT 0,
    player_chal               INTEGER NOT NULL DEFAULT 0,
    player_region             INTEGER NOT NULL DEFAULT 0,
    player_premium            INTEGER NOT NULL DEFAULT 0,
    player_type               INTEGER NOT NULL DEFAULT 0,
    player_developer          INTEGER NOT NULL DEFAULT 0,
    player_moderator          INTEGER NOT NULL DEFAULT 0,
    player_mapmod             INTEGER NOT NULL DEFAULT 0,
    player_featured           INTEGER NOT NULL DEFAULT 0,
    player_hack               INTEGER NOT NULL DEFAULT 0,
    player_infected           INTEGER NOT NULL DEFAULT 0,
    player_notrade            INTEGER NOT NULL DEFAULT 0,
    player_followed           INTEGER NOT NULL DEFAULT 0,
    player_following          INTEGER NOT NULL DEFAULT 0,
    player_clankills          INTEGER NOT NULL DEFAULT 0,
    player_clandeaths         INTEGER NOT NULL DEFAULT 0,
    partner_name              TEXT,
    partner_approved          INTEGER NOT NULL DEFAULT 0,
    tw_twitchname             TEXT,
    jobrating                 INTEGER NOT NULL DEFAULT 0,
    jobratingpositive         INTEGER NOT NULL DEFAULT 0,
    updated_at                TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX players_score_idx     ON players (player_score DESC);
CREATE INDEX players_kills_idx     ON players (player_kills DESC);
CREATE INDEX players_wins_idx      ON players (player_wins DESC);
CREATE INDEX players_time_idx      ON players (player_timeplayed DESC);
CREATE INDEX players_funds_idx     ON players (player_funds DESC);
CREATE INDEX players_elo_idx       ON players (player_elo DESC);
CREATE INDEX players_elo2_idx      ON players (player_elo2 DESC);
CREATE INDEX players_elo4_idx      ON players (player_elo4 DESC);
CREATE INDEX players_chal_idx      ON players (player_chal DESC);
CREATE INDEX players_followed_idx  ON players (player_followed DESC);
CREATE INDEX players_region_idx    ON players (player_region);

CREATE TABLE sessions (
    token_hash    TEXT PRIMARY KEY,
    account_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    last_seen_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    expires_at    TIMESTAMP NOT NULL,
    ip            TEXT,
    origin        TEXT
);
CREATE INDEX sessions_account_idx ON sessions (account_id);
CREATE INDEX sessions_expires_idx ON sessions (expires_at);

CREATE TABLE login_failures (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    username   TEXT NOT NULL,
    ip         TEXT NOT NULL,
    at         TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX login_failures_name_ip_idx ON login_failures (lower(username), ip, at);

CREATE TABLE login_ip_blocks (
    ip          TEXT PRIMARY KEY,
    failures    INTEGER NOT NULL DEFAULT 0,
    whitelist   BOOLEAN NOT NULL DEFAULT 0,
    updated_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);

CREATE TABLE login_audit (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    ip          TEXT,
    via         TEXT,
    at          TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX login_audit_account_idx ON login_audit (account_id, at DESC);

CREATE TABLE bans (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER REFERENCES accounts(id) ON DELETE CASCADE,
    ip          TEXT,
    reason      TEXT NOT NULL DEFAULT '',
    created_by  INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    expires_at  TIMESTAMP,
    revoked_at  TIMESTAMP,
    CHECK (account_id IS NOT NULL OR ip IS NOT NULL)
);
CREATE INDEX bans_account_idx ON bans (account_id) WHERE revoked_at IS NULL;
CREATE INDEX bans_ip_idx      ON bans (ip)         WHERE revoked_at IS NULL;

CREATE TABLE name_changes (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind        TEXT NOT NULL,
    old_name    TEXT,
    new_name    TEXT NOT NULL,
    at          TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX name_changes_account_idx ON name_changes (account_id, at DESC);

CREATE TABLE match_results (
    match_id    TEXT NOT NULL,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    server      TEXT,
    region      TEXT,
    map         TEXT,
    mode        TEXT,
    class_id    INTEGER,
    kills       INTEGER NOT NULL DEFAULT 0,
    deaths      INTEGER NOT NULL DEFAULT 0,
    score       INTEGER NOT NULL DEFAULT 0,
    won         BOOLEAN NOT NULL DEFAULT 0,
    games       INTEGER NOT NULL DEFAULT 0,
    time_ms     INTEGER NOT NULL DEFAULT 0,
    kr          INTEGER NOT NULL DEFAULT 0,
    stats       JSON TEXT NOT NULL DEFAULT '{}',
    at          TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (match_id, account_id)
);
CREATE INDEX match_results_account_idx ON match_results (account_id, at DESC);

CREATE TABLE kr_transactions (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    amount       INTEGER NOT NULL,
    source       TEXT NOT NULL,
    ref          TEXT,
    note         TEXT,
    at           TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX kr_transactions_account_idx ON kr_transactions (account_id, at DESC);
