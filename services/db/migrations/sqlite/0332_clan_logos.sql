-- 0332_clan_logos (SQLite): same table as postgres/0332_clan_logos (see there).
CREATE TABLE clan_logos (
    clan_id       INTEGER PRIMARY KEY REFERENCES clans(clan_id) ON DELETE CASCADE,
    status        TEXT NOT NULL CHECK (status IN ('pending', 'approved', 'rejected', 'error')),
    version       INTEGER NOT NULL DEFAULT 0,
    account_id    INTEGER,
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
CREATE INDEX clan_logos_status_idx ON clan_logos (status);
