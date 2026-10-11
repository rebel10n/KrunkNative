-- 0100_accounts (MySQL/MariaDB): credentials, profiles (Krunker v3.1.1 qs_player),
-- sessions, login audit / brute-force lockout, bans, alias history, match
-- results, KR ledger. Same tables and columns as postgres/0100 (+ 0105, 0106);
-- the multi-statement operations (postgres/0103 functions) are C in
-- services/brain/db_accounts.c. Conventions of the MySQL dialect (the driver
-- sets sql_mode ANSI_QUOTES,PIPES_AS_CONCAT,NO_BACKSLASH_ESCAPES and time_zone UTC):
--   timestamps  DATETIME(3) in UTC ({now} macro)
--   json        JSON (typed as JSON by the driver)
--   booleans    BOOLEAN (TINYINT(1)) 0/1
--   ids         BIGINT AUTO_INCREMENT
--   text keys   VARCHAR in the database's case-insensitive utf8mb4 collation,
--               so UNIQUE (username) also rejects other letter cases
--   sessions    token_hash = hex sha256 of the token ({hash:x} macro)
-- DDL commits implicitly in MySQL: a migration file that fails half way is not
-- rolled back (its kn_migrations row is only written on success).

CREATE TABLE accounts (
    id             BIGINT AUTO_INCREMENT PRIMARY KEY,
    username       VARCHAR(64) NOT NULL CHECK (length(username) BETWEEN 1 AND 32),
    password_hash  VARCHAR(255) NOT NULL,
    email          TEXT,
    birthday       VARCHAR(32),
    referral       TEXT,
    created_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    created_ip     TEXT,
    last_login_at  DATETIME(3),
    last_login_ip  TEXT,
    twofa_secret   VARCHAR(255),
    settings       MEDIUMTEXT,
    settings_at    DATETIME(3)
);
-- case-insensitive collation: UNIQUE (username) is lower(username)-unique
CREATE UNIQUE INDEX accounts_username_lower_uq ON accounts (username);

CREATE TABLE players (
    player_id                 BIGINT PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    player_alias              VARCHAR(64),
    player_kills              BIGINT NOT NULL DEFAULT 0,
    player_deaths             BIGINT NOT NULL DEFAULT 0,
    player_wins               BIGINT NOT NULL DEFAULT 0,
    player_games_played       BIGINT NOT NULL DEFAULT 0,
    player_score              BIGINT NOT NULL DEFAULT 0,
    player_timeplayed         BIGINT NOT NULL DEFAULT 0,
    player_funds              BIGINT NOT NULL DEFAULT 0 CHECK (player_funds >= 0),
    player_stats              JSON NOT NULL DEFAULT ('{}'),
    player_lastreward         BIGINT NOT NULL DEFAULT 0,
    player_lastfreespin       BIGINT NOT NULL DEFAULT 0,
    player_elo                BIGINT NOT NULL DEFAULT 0,
    player_elo2               BIGINT NOT NULL DEFAULT 0,
    player_elo4               BIGINT NOT NULL DEFAULT 0,
    player_chal               BIGINT NOT NULL DEFAULT 0,
    player_region             BIGINT NOT NULL DEFAULT 0,
    player_premium            BIGINT NOT NULL DEFAULT 0,
    player_type               BIGINT NOT NULL DEFAULT 0,
    player_developer          BIGINT NOT NULL DEFAULT 0,
    player_moderator          BIGINT NOT NULL DEFAULT 0,
    player_mapmod             BIGINT NOT NULL DEFAULT 0,
    player_featured           BIGINT NOT NULL DEFAULT 0,
    player_hack               BIGINT NOT NULL DEFAULT 0,
    player_infected           BIGINT NOT NULL DEFAULT 0,
    player_notrade            BIGINT NOT NULL DEFAULT 0,
    player_followed           BIGINT NOT NULL DEFAULT 0,
    player_following          BIGINT NOT NULL DEFAULT 0,
    player_clankills          BIGINT NOT NULL DEFAULT 0,
    player_clandeaths         BIGINT NOT NULL DEFAULT 0,
    partner_name              VARCHAR(64),
    partner_approved          BIGINT NOT NULL DEFAULT 0,
    tw_twitchname             VARCHAR(64),
    jobrating                 BIGINT NOT NULL DEFAULT 0,
    jobratingpositive         BIGINT NOT NULL DEFAULT 0,
    updated_at                DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
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
    token_hash    VARCHAR(64) PRIMARY KEY,
    account_id    BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    last_seen_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    expires_at    DATETIME(3) NOT NULL,
    ip            VARCHAR(64),
    origin        VARCHAR(32)
);
CREATE INDEX sessions_account_idx ON sessions (account_id);
CREATE INDEX sessions_expires_idx ON sessions (expires_at);

CREATE TABLE login_failures (
    id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    username   VARCHAR(64) NOT NULL,
    ip         VARCHAR(64) NOT NULL,
    at         DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX login_failures_name_ip_idx ON login_failures (username, ip, at);

CREATE TABLE login_ip_blocks (
    ip          VARCHAR(64) PRIMARY KEY,
    failures    BIGINT NOT NULL DEFAULT 0,
    whitelist   BOOLEAN NOT NULL DEFAULT 0,
    updated_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);

CREATE TABLE login_audit (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    ip          VARCHAR(64),
    via         VARCHAR(32),
    at          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX login_audit_account_idx ON login_audit (account_id, at DESC);

CREATE TABLE bans (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT REFERENCES accounts(id) ON DELETE CASCADE,
    ip          VARCHAR(64),
    reason      TEXT NOT NULL DEFAULT (''),
    created_by  BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    expires_at  DATETIME(3),
    revoked_at  DATETIME(3),
    CHECK (account_id IS NOT NULL OR ip IS NOT NULL)
);
CREATE INDEX bans_account_idx ON bans (account_id);
CREATE INDEX bans_ip_idx      ON bans (ip);

CREATE TABLE name_changes (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind        VARCHAR(32) NOT NULL,
    old_name    VARCHAR(128),
    new_name    VARCHAR(128) NOT NULL,
    at          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX name_changes_account_idx ON name_changes (account_id, at DESC);

CREATE TABLE match_results (
    match_id    VARCHAR(96) NOT NULL,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    server      VARCHAR(128),
    region      VARCHAR(64),
    map         VARCHAR(128),
    mode        VARCHAR(64),
    class_id    BIGINT,
    kills       BIGINT NOT NULL DEFAULT 0,
    deaths      BIGINT NOT NULL DEFAULT 0,
    score       BIGINT NOT NULL DEFAULT 0,
    won         BOOLEAN NOT NULL DEFAULT 0,
    games       BIGINT NOT NULL DEFAULT 0,
    time_ms     BIGINT NOT NULL DEFAULT 0,
    kr          BIGINT NOT NULL DEFAULT 0,
    stats       JSON NOT NULL DEFAULT ('{}'),
    at          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (match_id, account_id)
);
CREATE INDEX match_results_account_idx ON match_results (account_id, at DESC);

CREATE TABLE kr_transactions (
    id           BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    amount       BIGINT NOT NULL,
    source       VARCHAR(32) NOT NULL,
    ref          VARCHAR(128),
    note         TEXT,
    at           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX kr_transactions_account_idx ON kr_transactions (account_id, at DESC);
