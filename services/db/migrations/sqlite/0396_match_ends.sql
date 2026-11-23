-- 0396_match_ends (SQLite): same as postgres/0396. The end table (v10's endData.ed) of every finished match, which
-- "smr" (Share Match Results) posts as a MatchResult feed post (typ 8). Additive only.
CREATE TABLE match_ends (
    match_id    TEXT PRIMARY KEY,
    mode        INTEGER NOT NULL DEFAULT 0,
    ed          TEXT NOT NULL,
    ended_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
