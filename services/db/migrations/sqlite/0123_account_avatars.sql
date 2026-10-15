-- 0123_account_avatars (SQLite): same table as postgres/0123_account_avatars (see there).
CREATE TABLE account_avatars (
    account_id    INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    status        TEXT NOT NULL CHECK (status IN ('pending', 'approved', 'rejected', 'error')),
    sha256        TEXT NOT NULL DEFAULT '',
    image         TEXT,
    bytes         INTEGER NOT NULL DEFAULT 0,
    width         INTEGER NOT NULL DEFAULT 0,
    height        INTEGER NOT NULL DEFAULT 0,
    reason        TEXT NOT NULL DEFAULT '',
    categories    TEXT NOT NULL DEFAULT '',
    model         TEXT NOT NULL DEFAULT '',
    uploaded_at   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    moderated_at  TIMESTAMP
);
CREATE INDEX account_avatars_status_idx ON account_avatars (status);
