-- 0205_ranked_v10 (MySQL/MariaDB): same as postgres/0205; Krunker v10.0.2 ranked (doc/v10/ranked-impl.md). Owner: svc-matchmaker
-- (services/brain/ranked.c). Additive only: new tables plus nullable / defaulted columns on
-- the 0201 match tables; nothing is dropped or rewritten, so it is safe on the live database
-- while v7 brains still run (they keep using mm_ranked_parties / ranked_ratings).
--
-- v10 ratings are per (account, ranked season, ranked region): TrueSkill mu / sigma, shown as
-- 1000 + round(100 * (mu - 3 sigma)). Seasons are calendar months (season 1 = 2025-08 UTC,
-- 0 = the Beta season). The v7 per-season Elo rows (ranked_ratings) are carried over as the
-- Beta season (0) profiles of region "na" (every KrunkNative game server is in North America);
-- a player's first match of a later season starts from a soft reset of the latest profile.

CREATE TABLE ranked_profiles (
    account_id              BIGINT NOT NULL,
    season                  INT NOT NULL,
    region                  VARCHAR(8) NOT NULL,                -- as | eu | na
    mu                      DOUBLE NOT NULL DEFAULT 25,
    sigma                   DOUBLE NOT NULL DEFAULT 8.333333333333334,
    cr                      DOUBLE NOT NULL DEFAULT 0,   -- conservative_rating = mu - 3 sigma
    wins                    INT NOT NULL DEFAULT 0,
    losses                  INT NOT NULL DEFAULT 0,
    draws                   INT NOT NULL DEFAULT 0,
    abandons                INT NOT NULL DEFAULT 0,
    total_kills             BIGINT NOT NULL DEFAULT 0,
    total_deaths            BIGINT NOT NULL DEFAULT 0,
    total_assists           BIGINT NOT NULL DEFAULT 0,
    total_score             BIGINT NOT NULL DEFAULT 0,
    total_objective_score   BIGINT NOT NULL DEFAULT 0,
    total_damage_done       BIGINT NOT NULL DEFAULT 0,
    total_time_played       BIGINT NOT NULL DEFAULT 0,    -- ms
    highest_kill_count      INT NOT NULL DEFAULT 0,
    highest_damage_in_match INT NOT NULL DEFAULT 0,
    peak_mmr                INT NOT NULL DEFAULT 0,
    last_milestone_claimed  INT NOT NULL DEFAULT -1,
    last_match_at           DATETIME(3),
    updated_at              DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, season, region)
);
CREATE INDEX ranked_profiles_board_idx ON ranked_profiles (season, region, cr DESC);

-- Ranked Points (RP): balance + ledger
CREATE TABLE ranked_points (
    account_id    BIGINT PRIMARY KEY,
    balance       BIGINT NOT NULL DEFAULT 0 CHECK (balance >= 0),
    updated_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE TABLE ranked_points_tx (
    id            BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id    BIGINT NOT NULL,
    amount        BIGINT NOT NULL,
    source        VARCHAR(255) NOT NULL,                  -- match | milestone | season | purchase
    ref           VARCHAR(255) NOT NULL DEFAULT '',
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX ranked_points_tx_account_idx ON ranked_points_tx (account_id, id DESC);

-- end-of-season conversion, once per account and season
CREATE TABLE ranked_season_payouts (
    account_id    BIGINT NOT NULL,
    season        INT NOT NULL,
    region        VARCHAR(8) NOT NULL,
    points        BIGINT NOT NULL,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, season)
);

-- the solo queue (v10 MatchmakingService): one row per queued account
CREATE TABLE ranked_queue (
    account_id    BIGINT PRIMARY KEY,
    username      VARCHAR(255) NOT NULL,
    client_key    VARCHAR(96) NOT NULL UNIQUE,
    regions       VARCHAR(255) NOT NULL,                  -- "eu" or "as,eu,na"
    status        VARCHAR(16) NOT NULL DEFAULT 'QUEUED' CHECK (status IN ('QUEUED', 'MATCHED')),
    match_id      VARCHAR(255),
    assignment    TEXT,                           -- JSON {connection, gameId, clientId, webUrl, matchId}
    hold_until    DATETIME(3),                    -- not matched again before (a failed host)
    queued_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    last_poll     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX ranked_queue_status_idx ON ranked_queue (status, queued_at);

-- v10 match history on the 0201 match tables
-- the ruleset a match was played with (v7 matches: NULL; v7 Elo stays in ranked_ratings, v10 in ranked_profiles)
ALTER TABLE mm_ranked_matches ADD COLUMN ruleset VARCHAR(16);
ALTER TABLE mm_ranked_matches ADD COLUMN season INT;
ALTER TABLE mm_ranked_matches ADD COLUMN map_index INT;
ALTER TABLE mm_ranked_matches ADD COLUMN mode_index INT;
ALTER TABLE mm_ranked_match_players ADD COLUMN kills INT NOT NULL DEFAULT 0;
ALTER TABLE mm_ranked_match_players ADD COLUMN deaths INT NOT NULL DEFAULT 0;
ALTER TABLE mm_ranked_match_players ADD COLUMN assists INT NOT NULL DEFAULT 0;
ALTER TABLE mm_ranked_match_players ADD COLUMN score INT NOT NULL DEFAULT 0;
ALTER TABLE mm_ranked_match_players ADD COLUMN damage_done INT NOT NULL DEFAULT 0;
ALTER TABLE mm_ranked_match_players ADD COLUMN victory INT;          -- 1 win, 0 loss, NULL draw / open
ALTER TABLE mm_ranked_match_players ADD COLUMN abandoned BOOLEAN NOT NULL DEFAULT 0;
ALTER TABLE mm_ranked_match_players ADD COLUMN assignment TEXT;          -- JSON {connection, gameId, clientId, webUrl}: rejoin
ALTER TABLE mm_ranked_match_players ADD COLUMN rp INT NOT NULL DEFAULT 0;

-- the RP shop (GET /api/ranked/shop): rows are added by staff
CREATE TABLE ranked_shop (
    id            INT AUTO_INCREMENT PRIMARY KEY,
    skin_id       INT NOT NULL,
    price         INT NOT NULL CHECK (price >= 0),
    expires_at    DATETIME(3),
    max_purchase  INT NOT NULL DEFAULT 1,
    min_mmr       INT,
    max_mmr       INT,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE TABLE ranked_shop_purchases (
    account_id    BIGINT NOT NULL,
    shop_id       INT NOT NULL,
    count         INT NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, shop_id)
);

-- carry the v7 per-season Elo into the Beta season (ranked.h ranked_season_seed: +1 mu per 60
-- Elo of the best v7 season, at most +10, sigma 2/3 sigma0; the record summed over the seasons)
INSERT IGNORE INTO ranked_profiles (account_id, season, region, mu, sigma, cr, wins, losses, abandons, peak_mmr)
SELECT account_id, 0, 'na', 25 + LEAST(max(rating) / 60.0, 10), 5.555555555555555,
       25 + LEAST(max(rating) / 60.0, 10) - 16.666666666666664, sum(wins), sum(losses), sum(abandons),
       1000 + round(100 * (25 + LEAST(max(rating) / 60.0, 10) - 16.666666666666664))
  FROM ranked_ratings WHERE games > 0 GROUP BY account_id;
