-- 0346_kpd_demos (SQLite): same as postgres/0346; the K.P.D Demos window (v10 kpd.listDemos, tab 4).
-- Nothing writes it yet (no demo recorder); op kpd_demos reads it. Owner: services/brain/db_kpd.c.
-- Additive only.
CREATE TABLE kpd_demos (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    token          TEXT NOT NULL UNIQUE,
    ct             TEXT NOT NULL DEFAULT '',
    call_id        INTEGER REFERENCES kpd_calls(id) ON DELETE SET NULL,
    suspect_id     INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    suspect_name   TEXT NOT NULL DEFAULT '',
    suspect_group  INTEGER NOT NULL DEFAULT 0,
    typ            INTEGER NOT NULL DEFAULT 0,
    map            TEXT NOT NULL DEFAULT '',
    mode_name      TEXT NOT NULL DEFAULT '',
    duration_ms    INTEGER NOT NULL DEFAULT 0,
    banned         INTEGER NOT NULL DEFAULT 0,
    resolved       INTEGER NOT NULL DEFAULT 0,
    opened         INTEGER NOT NULL DEFAULT 1,
    yes_votes      INTEGER NOT NULL DEFAULT 0,
    no_votes       INTEGER NOT NULL DEFAULT 0,
    created_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX kpd_demos_created_idx ON kpd_demos (created_at);
CREATE INDEX kpd_demos_name_idx ON kpd_demos (lower(suspect_name));
