-- 0200_matchmaker (MySQL/MariaDB): cross-coordinator registry of brains, game server
-- nodes and their lobbies (postgres/0200 + 0203: tuple keeps its text as
-- written). The federation pub/sub falls back to polling table kn_pubsub
-- (knsvc/db.h).

CREATE TABLE mm_brains (
    brain_id     VARCHAR(128) PRIMARY KEY,
    region       VARCHAR(64) NOT NULL,
    public_url   VARCHAR(255) NOT NULL DEFAULT '',
    version      VARCHAR(64) NOT NULL DEFAULT '',
    started_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    heartbeat_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);

CREATE TABLE mm_nodes (
    node_id      VARCHAR(48) PRIMARY KEY CHECK (node_id REGEXP '^[A-Za-z0-9_.-]{1,47}$'),
    brain_id     VARCHAR(128) NOT NULL,
    region       VARCHAR(64) NOT NULL,
    name         VARCHAR(128) NOT NULL DEFAULT '',
    public_host  VARCHAR(255) NOT NULL DEFAULT '',
    public_port  BIGINT NOT NULL DEFAULT 0,
    web_url      VARCHAR(255) NOT NULL DEFAULT '',
    version      VARCHAR(64) NOT NULL DEFAULT '',
    max_players  BIGINT NOT NULL DEFAULT 8,
    max_lobbies  BIGINT NOT NULL DEFAULT 1,
    custom       BOOLEAN NOT NULL DEFAULT 0,
    draining     BOOLEAN NOT NULL DEFAULT 0,
    hello        JSON NOT NULL DEFAULT ('{}'),
    connected_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    heartbeat_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX mm_nodes_brain_idx ON mm_nodes (brain_id);
CREATE INDEX mm_nodes_heartbeat_idx ON mm_nodes (heartbeat_at);

CREATE TABLE mm_lobbies (
    game_id      VARCHAR(128) PRIMARY KEY,
    node_id      VARCHAR(48) NOT NULL REFERENCES mm_nodes (node_id) ON DELETE CASCADE,
    local_id     VARCHAR(64) NOT NULL,
    region       VARCHAR(64) NOT NULL,
    players      BIGINT NOT NULL DEFAULT 0,
    max_players  BIGINT NOT NULL DEFAULT 8,
    custom       BOOLEAN NOT NULL DEFAULT 0,
    protected    BOOLEAN NOT NULL DEFAULT 0,
    mode         VARCHAR(64) NOT NULL DEFAULT '',
    map          VARCHAR(128) NOT NULL DEFAULT '',
    version      VARCHAR(64) NOT NULL DEFAULT '',
    tuple        TEXT NOT NULL,                  -- [gameId, region, players, max, meta] as the node sent it
    updated_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX mm_lobbies_node_idx ON mm_lobbies (node_id);
CREATE INDEX mm_lobbies_region_idx ON mm_lobbies (region);

CREATE TABLE mm_reservations (
    jti          VARCHAR(128) PRIMARY KEY,
    game_id      VARCHAR(128) NOT NULL,
    brain_id     VARCHAR(128) NOT NULL,
    account_id   BIGINT,
    expires_at   DATETIME(3) NOT NULL
);
CREATE INDEX mm_reservations_game_idx ON mm_reservations (game_id, expires_at);

CREATE TABLE mm_host_requests (
    id           BIGINT AUTO_INCREMENT PRIMARY KEY,
    from_brain   VARCHAR(128) NOT NULL,
    target_brain VARCHAR(128) NOT NULL,
    target_node  VARCHAR(48) NOT NULL,
    region       VARCHAR(64) NOT NULL DEFAULT '',
    config       JSON NOT NULL DEFAULT ('{}'),
    host_account BIGINT,
    host_name    VARCHAR(64),
    status       VARCHAR(32) NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'sent', 'done', 'failed')),
    result       JSON,
    created_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX mm_host_requests_target_idx ON mm_host_requests (target_brain, status);

CREATE VIEW mm_live_lobbies AS
    SELECT l.game_id, l.region, n.brain_id, l.node_id, n.public_host, n.public_port,
           l.players, l.max_players, l.custom, l.mode, l.map, n.draining,
           n.heartbeat_at
      FROM mm_lobbies l JOIN mm_nodes n USING (node_id)
     WHERE n.heartbeat_at > NOW(3) - INTERVAL 30 SECOND;
