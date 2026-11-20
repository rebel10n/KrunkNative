-- 0390_rented_servers (MySQL / MariaDB): same as postgres/0390 (services/brain/rentals.c, doc/v10/rented-servers.md).
CREATE TABLE rented_servers (
    id           INT PRIMARY KEY,
    host         VARCHAR(64) NOT NULL,
    region       INT NOT NULL,
    svn          VARCHAR(64) NOT NULL,
    sid          INT NOT NULL,
    port         INT NOT NULL,
    public_host  VARCHAR(255) NOT NULL DEFAULT '',
    owner_id     BIGINT NULL,
    expires_s    BIGINT NOT NULL DEFAULT 0,
    conf         TEXT NULL,
    generation   INT NOT NULL DEFAULT 0,
    rent_gen     INT NOT NULL DEFAULT 0,
    rented_s     BIGINT NOT NULL DEFAULT 0,
    paid         BIGINT NOT NULL DEFAULT 0,
    running_gen  INT NOT NULL DEFAULT -1,
    state        VARCHAR(16) NULL,
    agent_seen_s BIGINT NOT NULL DEFAULT 0,
    FOREIGN KEY (owner_id) REFERENCES accounts(id) ON DELETE SET NULL
);
CREATE INDEX rented_servers_owner_idx ON rented_servers (owner_id);
CREATE INDEX rented_servers_host_idx ON rented_servers (host);
