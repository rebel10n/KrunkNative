-- 0204_ranked_seasons (MySQL/MariaDB): Krunker v7.1.4 ranked, one "ranked" queue and
-- ratings per season (postgres/0204). The queue id -> rating key mapping is C
-- (services/brain/db_mm.c).

CREATE TABLE ranked_ratings (
    account_id    BIGINT NOT NULL,
    season        INT NOT NULL,
    rating        INT NOT NULL DEFAULT 0,
    peak          INT NOT NULL DEFAULT 0,
    games         INT NOT NULL DEFAULT 0,
    wins          INT NOT NULL DEFAULT 0,
    losses        INT NOT NULL DEFAULT 0,
    abandons      INT NOT NULL DEFAULT 0,
    updated_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, season)
);
CREATE INDEX ranked_ratings_season_idx ON ranked_ratings (season, rating DESC);

DELETE FROM mm_ranked_parties WHERE status = 'Queued' AND NOT JSON_CONTAINS(queue_ids, '"ranked"');
