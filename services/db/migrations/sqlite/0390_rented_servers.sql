-- 0390_rented_servers (SQLite): same as postgres/0390 (services/brain/rentals.c, doc/v10/rented-servers.md).
CREATE TABLE rented_servers (
    id           INTEGER PRIMARY KEY,
    host         TEXT NOT NULL,
    region       INTEGER NOT NULL,
    svn          TEXT NOT NULL,
    sid          INTEGER NOT NULL,
    port         INTEGER NOT NULL,
    public_host  TEXT NOT NULL DEFAULT '',
    owner_id     INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    expires_s    INTEGER NOT NULL DEFAULT 0,
    conf         TEXT,
    generation   INTEGER NOT NULL DEFAULT 0,
    rent_gen     INTEGER NOT NULL DEFAULT 0,
    rented_s     INTEGER NOT NULL DEFAULT 0,
    paid         INTEGER NOT NULL DEFAULT 0,
    running_gen  INTEGER NOT NULL DEFAULT -1,
    state        TEXT,
    agent_seen_s INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX rented_servers_owner_idx ON rented_servers (owner_id);
CREATE INDEX rented_servers_host_idx ON rented_servers (host);
