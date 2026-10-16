-- 0332_clan_logos (MySQL / MariaDB): same table as postgres/0332_clan_logos (see there).
-- The image is MEDIUMTEXT: the base64 of a 512 px lossless WebP can pass TEXT's 64 KB limit.
CREATE TABLE clan_logos (
    clan_id       BIGINT PRIMARY KEY,
    status        VARCHAR(16) NOT NULL CHECK (status IN ('pending', 'approved', 'rejected', 'error')),
    version       BIGINT NOT NULL DEFAULT 0,
    account_id    BIGINT,                       -- who uploaded it (the approval's ledger row names them)
    sha256        VARCHAR(64) NOT NULL DEFAULT '',
    image         MEDIUMTEXT,
    bytes         BIGINT NOT NULL DEFAULT 0,
    width         BIGINT NOT NULL DEFAULT 0,
    height        BIGINT NOT NULL DEFAULT 0,
    reason        TEXT NOT NULL,
    categories    TEXT NOT NULL,
    model         VARCHAR(96) NOT NULL DEFAULT '',
    uploaded_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    moderated_at  DATETIME(3),
    CONSTRAINT clan_logos_clan_fk FOREIGN KEY (clan_id) REFERENCES clans(clan_id) ON DELETE CASCADE
);
CREATE INDEX clan_logos_status_idx ON clan_logos (status);
