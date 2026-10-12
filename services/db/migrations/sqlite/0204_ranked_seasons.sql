-- 0204_ranked_seasons (SQLite): Krunker v7.1.4 ranked, one "ranked" queue and ratings per
-- season (postgres/0204). The queue id -> rating key mapping is C (services/brain/db_mm.c).

CREATE TABLE ranked_ratings (
    account_id    INTEGER NOT NULL,
    season        INTEGER NOT NULL,
    rating        INTEGER NOT NULL DEFAULT 0,
    peak          INTEGER NOT NULL DEFAULT 0,
    games         INTEGER NOT NULL DEFAULT 0,
    wins          INTEGER NOT NULL DEFAULT 0,
    losses        INTEGER NOT NULL DEFAULT 0,
    abandons      INTEGER NOT NULL DEFAULT 0,
    updated_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, season)
);
CREATE INDEX ranked_ratings_season_idx ON ranked_ratings (season, rating DESC);

DELETE FROM mm_ranked_parties WHERE status = 'Queued'
   AND NOT EXISTS (SELECT 1 FROM json_each(mm_ranked_parties.queue_ids) WHERE json_each.value = 'ranked');
