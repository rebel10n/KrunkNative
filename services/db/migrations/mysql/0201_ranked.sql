-- 0201_ranked (MySQL/MariaDB): v3.1.1 ranked matchmaking queue (postgres/0201). The
-- queue functions are C (services/brain/db_ranked.c); the matcher's lock is the
-- row of mm_ranked_lock (SELECT ... FOR UPDATE NOWAIT inside its transaction).

CREATE TABLE mm_ranked_parties (
    party_id      VARCHAR(128) PRIMARY KEY,
    region        VARCHAR(64) NOT NULL,
    leader_client VARCHAR(128) NOT NULL,
    queue_ids     JSON NOT NULL DEFAULT ('[]'),   -- array of queue ids
    status        VARCHAR(32) NOT NULL DEFAULT 'Queued' CHECK (status IN ('Queued', 'CreatingGame', 'Join')),
    status_c      JSON NOT NULL DEFAULT ('{}'),
    match_id      VARCHAR(96),
    ready_since   DATETIME(3),
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);

CREATE TABLE mm_ranked_members (
    client_id     VARCHAR(128) PRIMARY KEY,
    client_key    VARCHAR(128) NOT NULL UNIQUE,
    party_id      VARCHAR(128) NOT NULL REFERENCES mm_ranked_parties (party_id) ON DELETE CASCADE,
    account_id    BIGINT NOT NULL UNIQUE,
    username      VARCHAR(64) NOT NULL,
    account_data  JSON NOT NULL DEFAULT ('{}'),
    class_index   BIGINT NOT NULL DEFAULT 0,
    is_ready      BOOLEAN NOT NULL DEFAULT 0,
    join_ticket   TEXT,                           -- signed seek ticket
    joined_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    last_poll     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX mm_ranked_members_party_idx ON mm_ranked_members (party_id);

CREATE TABLE mm_ranked_matches (
    match_id      VARCHAR(96) PRIMARY KEY,
    queue_id      VARCHAR(16) NOT NULL,
    region        VARCHAR(64) NOT NULL,
    brain_id      VARCHAR(128) NOT NULL,
    game_id       VARCHAR(128),
    status        VARCHAR(32) NOT NULL DEFAULT 'creating' CHECK (status IN ('creating', 'live', 'done', 'aborted')),
    result        JSON,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    started_at    DATETIME(3),
    finished_at   DATETIME(3)
);
CREATE INDEX mm_ranked_matches_status_idx ON mm_ranked_matches (status, created_at);

CREATE TABLE mm_ranked_match_players (
    match_id      VARCHAR(96) NOT NULL REFERENCES mm_ranked_matches (match_id) ON DELETE CASCADE,
    account_id    BIGINT NOT NULL,
    username      VARCHAR(64) NOT NULL,
    team          BIGINT NOT NULL,
    rating_key    VARCHAR(16) NOT NULL,
    rating_before BIGINT NOT NULL,
    rating_after  BIGINT,
    PRIMARY KEY (match_id, account_id)
);
CREATE INDEX mm_ranked_match_players_account_idx ON mm_ranked_match_players (account_id);

CREATE TABLE mm_ranked_bans (
    account_id    BIGINT PRIMARY KEY,
    until         DATETIME(3) NOT NULL,
    reason        TEXT NOT NULL DEFAULT ('')
);

-- the matcher's lock (postgres: pg_try_advisory_xact_lock(kn_rq_lock()))
CREATE TABLE mm_ranked_lock (
    id            BIGINT PRIMARY KEY
);
INSERT INTO mm_ranked_lock (id) VALUES (1);
