-- 0396_match_ends (MySQL / MariaDB): same as postgres/0396. The end table (v10's endData.ed) of every finished match,
-- which "smr" (Share Match Results) posts as a MatchResult feed post (typ 8). Additive only.
CREATE TABLE match_ends (
    match_id    VARCHAR(191) NOT NULL PRIMARY KEY,
    mode        INT NOT NULL DEFAULT 0,
    ed          MEDIUMTEXT NOT NULL,
    ended_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
