-- 0200_matchmaker (SQLite): cross-coordinator registry of brains, game server
-- nodes and their lobbies (postgres/0200 + 0203: tuple keeps its text as
-- written). Several coordinator processes may share one database file; the
-- federation pub/sub falls back to polling table kn_pubsub (knsvc/db.h).

CREATE TABLE mm_brains (
    brain_id     TEXT PRIMARY KEY,
    region       TEXT NOT NULL,
    public_url   TEXT NOT NULL DEFAULT '',
    version      TEXT NOT NULL DEFAULT '',
    started_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    heartbeat_at TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);

CREATE TABLE mm_nodes (
    node_id      TEXT PRIMARY KEY CHECK (length(node_id) BETWEEN 1 AND 47 AND node_id NOT GLOB '*[^A-Za-z0-9_.-]*'),
    brain_id     TEXT NOT NULL,
    region       TEXT NOT NULL,
    name         TEXT NOT NULL DEFAULT '',
    public_host  TEXT NOT NULL DEFAULT '',
    public_port  INTEGER NOT NULL DEFAULT 0,
    web_url      TEXT NOT NULL DEFAULT '',
    version      TEXT NOT NULL DEFAULT '',
    max_players  INTEGER NOT NULL DEFAULT 8,
    max_lobbies  INTEGER NOT NULL DEFAULT 1,
    custom       BOOLEAN NOT NULL DEFAULT 0,
    draining     BOOLEAN NOT NULL DEFAULT 0,
    hello        JSON TEXT NOT NULL DEFAULT '{}',
    connected_at TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    heartbeat_at TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX mm_nodes_brain_idx ON mm_nodes (brain_id);
CREATE INDEX mm_nodes_heartbeat_idx ON mm_nodes (heartbeat_at);

CREATE TABLE mm_lobbies (
    game_id      TEXT PRIMARY KEY,
    node_id      TEXT NOT NULL REFERENCES mm_nodes (node_id) ON DELETE CASCADE,
    local_id     TEXT NOT NULL,
    region       TEXT NOT NULL,
    players      INTEGER NOT NULL DEFAULT 0,
    max_players  INTEGER NOT NULL DEFAULT 8,
    custom       BOOLEAN NOT NULL DEFAULT 0,
    protected    BOOLEAN NOT NULL DEFAULT 0,
    mode         TEXT NOT NULL DEFAULT '',
    map          TEXT NOT NULL DEFAULT '',
    version      TEXT NOT NULL DEFAULT '',
    tuple        TEXT NOT NULL,                  -- [gameId, region, players, max, meta] as the node sent it
    updated_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX mm_lobbies_node_idx ON mm_lobbies (node_id);
CREATE INDEX mm_lobbies_region_idx ON mm_lobbies (region);

CREATE TABLE mm_reservations (
    jti          TEXT PRIMARY KEY,
    game_id      TEXT NOT NULL,
    brain_id     TEXT NOT NULL,
    account_id   INTEGER,
    expires_at   TIMESTAMP NOT NULL
);
CREATE INDEX mm_reservations_game_idx ON mm_reservations (game_id, expires_at);

CREATE TABLE mm_host_requests (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    from_brain   TEXT NOT NULL,
    target_brain TEXT NOT NULL,
    target_node  TEXT NOT NULL,
    region       TEXT NOT NULL DEFAULT '',
    config       JSON TEXT NOT NULL DEFAULT '{}',
    host_account INTEGER,
    host_name    TEXT,
    status       TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'sent', 'done', 'failed')),
    result       JSON TEXT,
    created_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    updated_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX mm_host_requests_target_idx ON mm_host_requests (target_brain, status);

CREATE VIEW mm_live_lobbies AS
    SELECT l.game_id, l.region, n.brain_id, l.node_id, n.public_host, n.public_port,
           l.players, l.max_players, l.custom, l.mode, l.map, n.draining,
           n.heartbeat_at
      FROM mm_lobbies l JOIN mm_nodes n USING (node_id)
     WHERE n.heartbeat_at > strftime('%Y-%m-%d %H:%M:%f', 'now', '-30 seconds');
