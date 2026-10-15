-- 0209_kpd_calls (SQLite): same as postgres/0209; the Krunker v10.0.2 K.P.D distress calls from the
-- in-game player list ("Call K.P.D"). Owner: services/brain/db_kpd.c. Additive only.
CREATE TABLE kpd_calls (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    reporter_id    INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    reporter_name  TEXT NOT NULL DEFAULT '',
    reporter_level INTEGER NOT NULL DEFAULT 0,
    suspect_id     INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    suspect_name   TEXT NOT NULL DEFAULT '',
    lobby          TEXT NOT NULL DEFAULT '',
    node           TEXT NOT NULL DEFAULT '',
    region         TEXT NOT NULL DEFAULT '',
    map            TEXT NOT NULL DEFAULT '',
    mode           INTEGER NOT NULL DEFAULT 0,
    typ            INTEGER NOT NULL DEFAULT 0,
    status         INTEGER NOT NULL DEFAULT 0,
    rating         INTEGER,
    handled_by     INTEGER,
    handled_at     TIMESTAMP,
    created_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX kpd_calls_open_idx ON kpd_calls (status, created_at);
CREATE INDEX kpd_calls_suspect_idx ON kpd_calls (suspect_id, created_at);
CREATE INDEX kpd_calls_reporter_idx ON kpd_calls (reporter_id, status);
