-- 0360_raid_stats (PostgreSQL): Krunker v10.0.3's raid record, kept apart from the profile stats (Raid is
-- noProfileStats, deob-10.0.3.js:118448, so its runs add no games / kills / deaths / wins): one row per account and
-- raid map (game.mapIndex: Tortuga 18, Laboratory 29, Soul Sanctum 31, Khepri 35, Bastion 43), the shape of v10's
-- acc.raidStats / profile raid_stats {map_id, completions, attempts, best_time, ai_kills, boss_kills, deaths}
-- (247596-247634, 125818-125839), and the same per UTC month for /leaderboards/raids/<map>/monthly?month=YYYY-MM
-- (31037-31047). best_time is the fastest clear in ms (NULL: never cleared). Written by the game servers' stats
-- commit (op "stats" with "raid", once per match through the commit's duplicate check; services/brain/raids.c).
-- Additive only.
CREATE TABLE raid_stats (
    account_id   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    map_id       integer NOT NULL,
    completions  integer NOT NULL DEFAULT 0 CHECK (completions >= 0),
    attempts     integer NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    best_time    bigint CHECK (best_time IS NULL OR best_time > 0),
    ai_kills     bigint NOT NULL DEFAULT 0 CHECK (ai_kills >= 0),
    boss_kills   bigint NOT NULL DEFAULT 0 CHECK (boss_kills >= 0),
    deaths       bigint NOT NULL DEFAULT 0 CHECK (deaths >= 0),
    updated_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, map_id)
);
CREATE INDEX raid_stats_time_idx ON raid_stats (map_id, best_time);
CREATE INDEX raid_stats_clears_idx ON raid_stats (map_id, completions DESC);

CREATE TABLE raid_stats_monthly (
    account_id   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    map_id       integer NOT NULL,
    month        text NOT NULL CHECK (month ~ '^[0-9]{4}-[0-9]{2}$'),
    completions  integer NOT NULL DEFAULT 0 CHECK (completions >= 0),
    attempts     integer NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    best_time    bigint CHECK (best_time IS NULL OR best_time > 0),
    ai_kills     bigint NOT NULL DEFAULT 0 CHECK (ai_kills >= 0),
    boss_kills   bigint NOT NULL DEFAULT 0 CHECK (boss_kills >= 0),
    deaths       bigint NOT NULL DEFAULT 0 CHECK (deaths >= 0),
    updated_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, map_id, month)
);
CREATE INDEX raid_stats_monthly_time_idx ON raid_stats_monthly (map_id, month, best_time);
CREATE INDEX raid_stats_monthly_clears_idx ON raid_stats_monthly (map_id, month, completions DESC);
