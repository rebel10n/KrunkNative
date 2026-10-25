-- 0360_raid_stats (MySQL / MariaDB): same as postgres/0360. Krunker v10.0.3's raid record per account and raid
-- map (game.mapIndex), all time and per UTC month (services/brain/raids.c). Additive only.
CREATE TABLE raid_stats (
    account_id   BIGINT NOT NULL,
    map_id       INT NOT NULL,
    completions  INT NOT NULL DEFAULT 0 CHECK (completions >= 0),
    attempts     INT NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    best_time    BIGINT NULL CHECK (best_time IS NULL OR best_time > 0),
    ai_kills     BIGINT NOT NULL DEFAULT 0 CHECK (ai_kills >= 0),
    boss_kills   BIGINT NOT NULL DEFAULT 0 CHECK (boss_kills >= 0),
    deaths       BIGINT NOT NULL DEFAULT 0 CHECK (deaths >= 0),
    updated_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, map_id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX raid_stats_time_idx ON raid_stats (map_id, best_time);
CREATE INDEX raid_stats_clears_idx ON raid_stats (map_id, completions);

CREATE TABLE raid_stats_monthly (
    account_id   BIGINT NOT NULL,
    map_id       INT NOT NULL,
    month        CHAR(7) NOT NULL,
    completions  INT NOT NULL DEFAULT 0 CHECK (completions >= 0),
    attempts     INT NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    best_time    BIGINT NULL CHECK (best_time IS NULL OR best_time > 0),
    ai_kills     BIGINT NOT NULL DEFAULT 0 CHECK (ai_kills >= 0),
    boss_kills   BIGINT NOT NULL DEFAULT 0 CHECK (boss_kills >= 0),
    deaths       BIGINT NOT NULL DEFAULT 0 CHECK (deaths >= 0),
    updated_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, map_id, month),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX raid_stats_monthly_time_idx ON raid_stats_monthly (map_id, month, best_time);
CREATE INDEX raid_stats_monthly_clears_idx ON raid_stats_monthly (map_id, month, completions);
