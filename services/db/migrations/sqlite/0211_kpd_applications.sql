-- 0211_kpd_applications (SQLite): same as postgres/0211; K.P.D applications ("kpdA").
-- Owner: services/brain/db_kpd.c. Additive only.
CREATE TABLE kpd_applications (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name        TEXT NOT NULL DEFAULT '',
    discord     TEXT NOT NULL DEFAULT '',
    reason      TEXT NOT NULL DEFAULT '',
    status      INTEGER NOT NULL DEFAULT 0,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    closed_at   TIMESTAMP
);
CREATE INDEX kpd_applications_status_idx ON kpd_applications (status, created_at);
CREATE INDEX kpd_applications_account_idx ON kpd_applications (account_id, status);
