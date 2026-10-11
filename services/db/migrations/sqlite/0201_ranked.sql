-- 0201_ranked (SQLite): v3.1.1 ranked matchmaking queue (postgres/0201). The
-- queue functions are C (services/brain/db_ranked.c); the matcher's lock is the
-- BEGIN IMMEDIATE write lock of its transaction.

CREATE TABLE mm_ranked_parties (
    party_id      TEXT PRIMARY KEY,
    region        TEXT NOT NULL,
    leader_client TEXT NOT NULL,
    queue_ids     JSON TEXT NOT NULL DEFAULT '[]',   -- array of queue ids
    status        TEXT NOT NULL DEFAULT 'Queued' CHECK (status IN ('Queued', 'CreatingGame', 'Join')),
    status_c      JSON TEXT NOT NULL DEFAULT '{}',
    match_id      TEXT,
    ready_since   TIMESTAMP,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    updated_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);

CREATE TABLE mm_ranked_members (
    client_id     TEXT PRIMARY KEY,
    client_key    TEXT NOT NULL UNIQUE,
    party_id      TEXT NOT NULL REFERENCES mm_ranked_parties (party_id) ON DELETE CASCADE,
    account_id    INTEGER NOT NULL UNIQUE,
    username      TEXT NOT NULL,
    account_data  JSON TEXT NOT NULL DEFAULT '{}',
    class_index   INTEGER NOT NULL DEFAULT 0,
    is_ready      BOOLEAN NOT NULL DEFAULT 0,
    join_ticket   TEXT,
    joined_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    last_poll     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX mm_ranked_members_party_idx ON mm_ranked_members (party_id);

CREATE TABLE mm_ranked_matches (
    match_id      TEXT PRIMARY KEY,
    queue_id      TEXT NOT NULL,
    region        TEXT NOT NULL,
    brain_id      TEXT NOT NULL,
    game_id       TEXT,
    status        TEXT NOT NULL DEFAULT 'creating' CHECK (status IN ('creating', 'live', 'done', 'aborted')),
    result        JSON TEXT,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    started_at    TIMESTAMP,
    finished_at   TIMESTAMP
);
CREATE INDEX mm_ranked_matches_status_idx ON mm_ranked_matches (status, created_at);

CREATE TABLE mm_ranked_match_players (
    match_id      TEXT NOT NULL REFERENCES mm_ranked_matches (match_id) ON DELETE CASCADE,
    account_id    INTEGER NOT NULL,
    username      TEXT NOT NULL,
    team          INTEGER NOT NULL,
    rating_key    TEXT NOT NULL,
    rating_before INTEGER NOT NULL,
    rating_after  INTEGER,
    PRIMARY KEY (match_id, account_id)
);
CREATE INDEX mm_ranked_match_players_account_idx ON mm_ranked_match_players (account_id);

CREATE TABLE mm_ranked_bans (
    account_id    INTEGER PRIMARY KEY,
    until         TIMESTAMP NOT NULL,
    reason        TEXT NOT NULL DEFAULT ''
);
