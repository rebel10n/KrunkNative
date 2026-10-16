-- 0212_turf_rounds (SQLite): same as postgres/0212; the Turf Wars rounds already counted (match id).
-- Owner: services/brain/db_turf.c. Additive only.
CREATE TABLE turf_rounds (
    match_id     TEXT PRIMARY KEY,
    day          INTEGER NOT NULL,
    map_id       INTEGER NOT NULL,
    recorded_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
