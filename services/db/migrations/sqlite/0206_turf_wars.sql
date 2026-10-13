-- 0206_turf_wars (SQLite): same as postgres/0206; Krunker v10.0.2 Turf Wars ("territorialWars", v10's clan war;
-- doc/v10/ranked-impl.md). Owner: svc-social (services/brain/turf.c, db_turf.c). Additive only:
-- new tables, no change to the 0101 clan tables, safe on the live database.
--
-- Game servers report the counted clan kills of each official public round (internal WS
-- "turf-kills"); a brain settles every finished day once (turf_days): per map the clan with the
-- most kills (top 20 contributors) controls it the next day, the day's kills become a KR pool
-- that the top 3 clans split 60 / 30 / 10, each clan's cut shared by its loot config.

-- per (day, map, player): the player's counted kills / deaths, with the clan at the time
CREATE TABLE turf_kills (
    day           INTEGER NOT NULL,              -- days since 2026-03-17 UTC
    map_id        INTEGER NOT NULL,              -- 0 Burg, 2 Sandstorm, 3 Subzero
    account_id    INTEGER NOT NULL,
    clan_id       INTEGER NOT NULL,
    kills         INTEGER NOT NULL DEFAULT 0,
    deaths        INTEGER NOT NULL DEFAULT 0,
    games         INTEGER NOT NULL DEFAULT 0,
    updated_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (day, map_id, account_id, clan_id)
);
CREATE INDEX turf_kills_clan_idx ON turf_kills (clan_id, day);

-- a settled day per map: the winner (controls the map on day + 1) and the pool
CREATE TABLE turf_days (
    day           INTEGER NOT NULL,
    map_id        INTEGER NOT NULL,
    clan_id       INTEGER,                       -- NULL: nobody played
    kills         INTEGER NOT NULL DEFAULT 0,    -- the winner's counted kills
    total_kills   INTEGER NOT NULL DEFAULT 0,    -- every counted kill on the map
    pool          INTEGER NOT NULL DEFAULT 0,     -- KR
    settled_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (day, map_id)
);

-- the per-clan results of a settled day (positions 1..n by counted kills)
CREATE TABLE turf_clan_days (
    day           INTEGER NOT NULL,
    map_id        INTEGER NOT NULL,
    clan_id       INTEGER NOT NULL,
    position      INTEGER NOT NULL,
    kills         INTEGER NOT NULL,
    kr            INTEGER NOT NULL DEFAULT 0,     -- the clan's cut of the pool
    PRIMARY KEY (day, map_id, clan_id)
);
CREATE INDEX turf_clan_days_clan_idx ON turf_clan_days (clan_id, day);

-- individual payouts of a clan's cut (loot config), and the clan bank's share
CREATE TABLE turf_payouts (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    day           INTEGER NOT NULL,
    map_id        INTEGER NOT NULL,
    clan_id       INTEGER NOT NULL,
    account_id    INTEGER,                        -- NULL: the clan bank
    kr            INTEGER NOT NULL,
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX turf_payouts_clan_idx ON turf_payouts (clan_id, day);
CREATE INDEX turf_payouts_account_idx ON turf_payouts (account_id, day);

-- a clan's Turf Wars loot distribution (v10 /clans/<id>/loot-config)
CREATE TABLE turf_loot_config (
    clan_id              INTEGER PRIMARY KEY,
    distribution_mode    TEXT NOT NULL DEFAULT 'clan_bank',
    clan_pct             INTEGER NOT NULL DEFAULT 0,
    top_player_count     INTEGER NOT NULL DEFAULT 20,
    top_player_pct       INTEGER NOT NULL DEFAULT 80,
    remainder_pct        INTEGER NOT NULL DEFAULT 20,
    min_games_required   INTEGER NOT NULL DEFAULT 1,
    updated_by           TEXT NOT NULL DEFAULT '',
    updated_at           TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
