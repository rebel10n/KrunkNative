-- 0123_account_avatars (MySQL / MariaDB): same table as postgres/0123_account_avatars (see there).
-- The image is MEDIUMTEXT: the base64 of a 256 px WebP can pass TEXT's 64 KB limit.
CREATE TABLE account_avatars (
    account_id    BIGINT PRIMARY KEY,
    status        VARCHAR(16) NOT NULL CHECK (status IN ('pending', 'approved', 'rejected', 'error')),
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
    CONSTRAINT account_avatars_account_fk FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX account_avatars_status_idx ON account_avatars (status);
