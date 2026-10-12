-- 0204_ranked_seasons: Krunker v7.1.4 ranked (doc/backend.md 6.5). Owner: svc-matchmaker
-- (agent ranked-v7). One queue, "ranked" (v7's competitive 4v4), instead of v3.1.1's
-- r1v1 / r2v2 / r4v4, and ratings per season: ranked_ratings, keyed by the season int of
-- accounts.current_season (the seasons registry is season-system's 0108; this table only
-- reads the number). players.player_elo mirrors the current season's rating; player_elo2 /
-- player_elo4 are no longer written by ranked (v7's player_elo4 is the Junk stat).

CREATE TABLE ranked_ratings (
    account_id    bigint NOT NULL,
    season        integer NOT NULL,
    rating        integer NOT NULL DEFAULT 0,   -- floored at 0 (v3.1.1 updateELO)
    peak          integer NOT NULL DEFAULT 0,
    games         integer NOT NULL DEFAULT 0,
    wins          integer NOT NULL DEFAULT 0,
    losses        integer NOT NULL DEFAULT 0,
    abandons      integer NOT NULL DEFAULT 0,   -- rankedDeduction + rankedCooldown each
    updated_at    timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, season)
);
CREATE INDEX ranked_ratings_season_idx ON ranked_ratings (season, rating DESC);

-- the queue id -> the members' rating key (account_data.elo: the season rating) / team size
CREATE OR REPLACE FUNCTION kn_rq_key(q text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE q WHEN 'ranked' THEN 'elo' END
$$;
CREATE OR REPLACE FUNCTION kn_rq_team_size(q text) RETURNS integer LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE q WHEN 'ranked' THEN 4 END
$$;

-- queue entries of the removed v3.1.1 queues can never match again
DELETE FROM mm_ranked_parties WHERE status = 'Queued' AND NOT ('ranked' = ANY (queue_ids));
