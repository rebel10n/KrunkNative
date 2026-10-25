-- 0360_raid_stats (SQLite): same as postgres/0360. Krunker v10.0.3's raid record per account and raid map
-- (game.mapIndex), all time and per UTC month (services/brain/raids.c). Additive only.
CREATE TABLE raid_stats (
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    map_id       INTEGER NOT NULL,
    completions  INTEGER NOT NULL DEFAULT 0 CHECK (completions >= 0),
    attempts     INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    best_time    INTEGER CHECK (best_time IS NULL OR best_time > 0),
    ai_kills     INTEGER NOT NULL DEFAULT 0 CHECK (ai_kills >= 0),
    boss_kills   INTEGER NOT NULL DEFAULT 0 CHECK (boss_kills >= 0),
    deaths       INTEGER NOT NULL DEFAULT 0 CHECK (deaths >= 0),
    updated_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, map_id)
);
CREATE INDEX raid_stats_time_idx ON raid_stats (map_id, best_time);
CREATE INDEX raid_stats_clears_idx ON raid_stats (map_id, completions DESC);

CREATE TABLE raid_stats_monthly (
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    map_id       INTEGER NOT NULL,
    month        TEXT NOT NULL CHECK (length(month) = 7),
    completions  INTEGER NOT NULL DEFAULT 0 CHECK (completions >= 0),
    attempts     INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    best_time    INTEGER CHECK (best_time IS NULL OR best_time > 0),
    ai_kills     INTEGER NOT NULL DEFAULT 0 CHECK (ai_kills >= 0),
    boss_kills   INTEGER NOT NULL DEFAULT 0 CHECK (boss_kills >= 0),
    deaths       INTEGER NOT NULL DEFAULT 0 CHECK (deaths >= 0),
    updated_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, map_id, month)
);
CREATE INDEX raid_stats_monthly_time_idx ON raid_stats_monthly (map_id, month, best_time);
CREATE INDEX raid_stats_monthly_clears_idx ON raid_stats_monthly (map_id, month, completions DESC);
