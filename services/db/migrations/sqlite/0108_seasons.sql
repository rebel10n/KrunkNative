-- 0108_seasons (SQLite): same tables as postgres/0108_seasons (see there).

-- One row per season; the current one is config accounts.current_season.
CREATE TABLE seasons (
    season      INTEGER PRIMARY KEY CHECK (season > 0),
    name        TEXT NOT NULL DEFAULT '',
    starts_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    ends_at     TIMESTAMP,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
INSERT INTO seasons (season, name) VALUES (7, 'Season 7') ON CONFLICT (season) DO NOTHING;

-- qs_battlepass: one pass per season (the active one: status 1, highest bp_id). itemdata = v7's
-- "id,tier,col,sk" groups (tier 0 free / 1 VIP, col = level - 1, sk = store.skins index).
-- KrunkNative: the VIP tier and skipped levels cost KR (v7: tier1price / levelSkipPrice in USD).
CREATE TABLE battlepasses (
    bp_id          INTEGER PRIMARY KEY CHECK (bp_id > 0),
    season         INTEGER NOT NULL REFERENCES seasons(season),
    name           TEXT NOT NULL,
    descr          TEXT NOT NULL DEFAULT '',
    status         INTEGER NOT NULL DEFAULT 1,
    bpp_perlevel   INTEGER NOT NULL CHECK (bpp_perlevel > 0),
    itemcount      INTEGER NOT NULL CHECK (itemcount > 0),
    tier1_name     TEXT NOT NULL DEFAULT 'VIP',
    tier1_price_kr INTEGER NOT NULL DEFAULT 1200 CHECK (tier1_price_kr >= 0),
    skip_price_kr  INTEGER NOT NULL DEFAULT 200 CHECK (skip_price_kr >= 0),
    itemdata       TEXT NOT NULL DEFAULT '',
    startdate      TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    enddate        TIMESTAMP NOT NULL
);
CREATE INDEX battlepasses_season_idx ON battlepasses (season, status);

-- qs_battlepass_transactions: account.BP {bpp, tier, items_claimed} per pass; tier_at = when the
-- VIP tier was bought (the Founder badges).
CREATE TABLE battlepass_progress (
    account_id     INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    bp_id          INTEGER NOT NULL REFERENCES battlepasses(bp_id) ON DELETE CASCADE,
    bpp            INTEGER NOT NULL DEFAULT 0 CHECK (bpp >= 0),
    tier           INTEGER NOT NULL DEFAULT 0 CHECK (tier >= 0),
    items_claimed  TEXT NOT NULL DEFAULT '',
    tier_at        TIMESTAMP,
    updated_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, bp_id)
);

-- Per-season totals of counted public matches (the commit's kills / deaths / win / game /
-- score / time played): the season leaderboards and the profile's season block.
CREATE TABLE season_stats (
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    season       INTEGER NOT NULL REFERENCES seasons(season),
    kills        INTEGER NOT NULL DEFAULT 0,
    deaths       INTEGER NOT NULL DEFAULT 0,
    wins         INTEGER NOT NULL DEFAULT 0,
    games        INTEGER NOT NULL DEFAULT 0,
    score        INTEGER NOT NULL DEFAULT 0,
    time_played  INTEGER NOT NULL DEFAULT 0,
    updated_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, season)
);
CREATE INDEX season_stats_score_idx ON season_stats (season, score DESC);

-- The Season 7 pass (currentBattlepass 2). v7's rows lived in its database; this one follows
-- Season 7 as it shipped (v7.0.0, 2023-04-04): 50 levels and 100 items (Krunker wiki "Battle
-- Pass", Season 7 tab, revisions 24386-24389: every level's free and VIP reward, the non-skin ones
-- being the Season 7 Starter / Elite / Heroic / Ultra tickets, store 6423-6426), 300 BPP per level
-- (= config.bppCapDay; 50 x 300 = the 15000 of the S7 Battle Pass Master badge; the per-level
-- value is not published). Item ids: 20000 + level (free), 21000 + level (VIP). KrunkNative
-- prices in KR (v7: VIP $5.99, a level $0.99; ~200 KR per USD): 1200 and 200. The pass starts
-- when this migration runs and lasts 180 days.
INSERT INTO battlepasses (bp_id, season, name, descr, bpp_perlevel, itemcount, tier1_name, tier1_price_kr, skip_price_kr, itemdata, startdate, enddate) VALUES (2, 7, 'Season 7 Battle Pass', 'Krunker Season 7', 300, 50, 'VIP', 1200, 200,
    '20001,0,0,6316,20002,0,1,6424,20003,0,2,6424,20004,0,3,6315,20005,0,4,6423,20006,0,5,6268,20007,0,6,6423,20008,0,7,6424,20009,0,8,6259,20010,0,9,6424,20011,0,10,6260,20012,0,11,6423,20013,0,12,6424,20014,0,13,6424,20015,0,14,6423,20016,0,15,6424,20017,0,16,6288,20018,0,17,6423,20019,0,18,6424,20020,0,19,6304,20021,0,20,6423,20022,0,21,6424,20023,0,22,6423,20024,0,23,6285,20025,0,24,6287,20026,0,25,6424,20027,0,26,6423,20028,0,27,6424,20029,0,28,6423,20030,0,29,6424,20031,0,30,6423,20032,0,31,6305,20033,0,32,6286,20034,0,33,6424,20035,0,34,6423,20036,0,35,6424,20037,0,36,6423,20038,0,37,6284,20039,0,38,6423,20040,0,39,6424,20041,0,40,6423,20042,0,41,6424,20043,0,42,6423,20044,0,43,6424,20045,0,44,6303,20046,0,45,6423,20047,0,46,6302,20048,0,47,6424,20049,0,48,6424,20050,0,49,6254,21001,1,0,6425,21002,1,1,6265,21003,1,2,6425,21004,1,3,6314,21005,1,4,6425,21006,1,5,6257,21007,1,6,6425,21008,1,7,6309,21009,1,8,6312,21010,1,9,6425,21011,1,10,6266,21012,1,11,6424,21013,1,12,6425,21014,1,13,6310,21015,1,14,6424,21016,1,15,6425,21017,1,16,6258,21018,1,17,6424,21019,1,18,6426,21020,1,19,6307,21021,1,20,6424,21022,1,21,6261,21023,1,22,6313,21024,1,23,6424,21025,1,24,6300,21026,1,25,6425,21027,1,26,6424,21028,1,27,6306,21029,1,28,6424,21030,1,29,6308,21031,1,30,6425,21032,1,31,6425,21033,1,32,6267,21034,1,33,6425,21035,1,34,6262,21036,1,35,6311,21037,1,36,6425,21038,1,37,6298,21039,1,38,6264,21040,1,39,6426,21041,1,40,6263,21042,1,41,6425,21043,1,42,6426,21044,1,43,6425,21045,1,44,6299,21046,1,45,6290,21047,1,46,6426,21048,1,47,6289,21049,1,48,6381,21050,1,49,6269',
    strftime('%Y-%m-%d %H:%M:%f', 'now'), strftime('%Y-%m-%d %H:%M:%f', 'now', '+180 days'))
    ON CONFLICT (bp_id) DO NOTHING;
