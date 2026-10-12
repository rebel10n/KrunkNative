-- 0107_progression (MySQL/MariaDB): same tables as postgres/0107 (the account JSON is C there).
-- player-viewer WP-E part 2c (doc/v7/player-viewer.md 1.4.2-1.4.4): Krunker
-- v7.1.4 badges (src/data/badges.js; v7's player_badge / qs_player_badgeallocation, procs 181),
-- data-driven challenges (src/data/challenges.js activeChallenges {CI, T, DAT, REW, PV, PT}; v7
-- procs 175 / 176) and per-account medal counts (game.incMedal, endData.mdls). Read and written
-- by kn_brain's C procs in services/brain/db_progress.c (every driver); the account object gains
-- "badges" (db_account_json in services/brain/db_common.c).
-- Owner: svc-accounts.

-- Earned badges (badges.js indices). The selected one stays players.player_stats.bdg (v7 stats.bdg).
CREATE TABLE player_badges (
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    badge       INTEGER NOT NULL CHECK (badge >= 0),
    claimed_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, badge)
);

-- Challenge definitions: T (ctype) daily / weekly / map / weapon / event / nightmare / ranked /
-- season / clan, DAT and REW as v7's JSON texts, IC (icon: img/chals/icon_<n>.png), N (name; ''
-- = i18n chall.name.<ky>). Daily and weekly rows rotate: each account gets a few of them per
-- UTC day / week (db_progress.c); the others are active until completed.
CREATE TABLE challenges (
    challenge_id BIGINT PRIMARY KEY CHECK (challenge_id > 0),
    ctype        VARCHAR(16) NOT NULL,
    dat          VARCHAR(1024) NOT NULL,
    rew          VARCHAR(1024) NOT NULL DEFAULT '{}',
    icon         INTEGER NOT NULL DEFAULT 0,
    name         VARCHAR(128) NOT NULL DEFAULT '',
    active       SMALLINT NOT NULL DEFAULT 1,
    sort         INTEGER NOT NULL DEFAULT 0
);

-- Progress (PV) and completion (PT) per account, challenge and period (UTC day / ISO-like week
-- number for daily / weekly, 0 otherwise).
CREATE TABLE player_challenges (
    account_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    challenge_id BIGINT NOT NULL REFERENCES challenges(challenge_id) ON DELETE CASCADE,
    period       INTEGER NOT NULL DEFAULT 0,
    pv           BIGINT NOT NULL DEFAULT 0,
    pt           SMALLINT NOT NULL DEFAULT 0,
    updated_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, challenge_id, period)
);

-- Medals earned over every committed match (overlay.medalsList names).
CREATE TABLE player_medals (
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    medal       VARCHAR(32) NOT NULL,
    count       BIGINT NOT NULL DEFAULT 0 CHECK (count >= 0),
    PRIMARY KEY (account_id, medal)
);

-- The starting challenge set (KrunkNative's own: v7's rows lived in its database).
INSERT INTO challenges (challenge_id, ctype, dat, rew, icon, name, sort) VALUES
    (1, 'map', '{"ky":"k","val":25,"mp":0}', '{"kr":10}', 0, 'Get 25 kills on Burg', 10),
    (2, 'map', '{"ky":"hs","val":10,"mp":0}', '{"kr":15}', 1, 'Get 10 headshots on Burg', 11),
    (3, 'map', '{"ky":"k","val":25,"mp":2}', '{"kr":10}', 0, 'Get 25 kills on Sandstorm', 12),
    (4, 'map', '{"ky":"w","val":3,"mp":2}', '{"kr":20}', 2, 'Win 3 matches on Sandstorm', 13),
    (5, 'map', '{"ky":"k","val":25,"mp":10}', '{"kr":10}', 0, 'Get 25 kills on Kanji', 14),
    (6, 'map', '{"ky":"score","val":5000,"mp":10}', '{"kr":15}', 3, 'Get 5000 score on Kanji', 15),
    (7, 'weapon', '{"ky":"ns","val":10,"wep":0}', '{"kr":15}', 4, 'Get 10 no-scopes with the Sniper Rifle', 20),
    (8, 'weapon', '{"ky":"qs","val":10,"wep":0}', '{"kr":15}', 4, 'Get 10 quickscopes with the Sniper Rifle', 21),
    (9, 'weapon', '{"ky":"k","val":50,"wep":1}', '{"kr":15}', 5, 'Get 50 kills with the Assault Rifle', 22),
    (10, 'weapon', '{"ky":"hs","val":15,"wep":1}', '{"kr":20}', 5, 'Get 15 headshots with the Assault Rifle', 23),
    (11, 'weapon', '{"ky":"k","val":50,"wep":3}', '{"kr":15}', 6, 'Get 50 kills with the Submachine Gun', 24),
    (12, 'weapon', '{"ky":"mk","val":10,"wep":12}', '{"kr":25}', 7, 'Get 10 melee kills with the Combat Knife', 25),
    (13, 'nightmare', '{"ky":"kstk","val":15}', '{"kr":50}', 8, 'Get a 15 killstreak', 30),
    (14, 'nightmare', '{"ky":"score","val":10000,"sm":1,"boss":1}', '{"kr":100}', 9, 'Get 10000 score in a single match', 31),
    (15, 'daily', '{"ky":"hs","val":15}', '{"kr":5}', 1, '', 40),
    (16, 'daily', '{"ky":"score","val":3000}', '{"kr":5}', 3, '', 41),
    (17, 'daily', '{"ky":"w","val":2}', '{"kr":10}', 2, '', 42),
    (18, 'daily', '{"ky":"hs","val":5,"wep":0}', '{"kr":10}', 4, '', 43),
    (19, 'daily', '{"ky":"d","val":20}', '{"kr":5}', 0, '', 44),
    (20, 'daily', '{"ky":"spry","val":5}', '{"kr":5}', 0, 'Place 5 sprays', 45),
    (21, 'daily', '{"ky":"ast","val":10}', '{"kr":5}', 0, 'Get 10 assists', 46),
    (22, 'daily', '{"ky":"wb","val":3}', '{"kr":10}', 0, '', 47),
    (23, 'weekly', '{"ky":"k","val":250}', '{"kr":50}', 0, 'Get 250 kills', 50),
    (24, 'weekly', '{"ky":"w","val":15}', '{"kr":50}', 2, '', 51),
    (25, 'weekly', '{"ky":"score","val":50000}', '{"kr":50}', 3, '', 52),
    (26, 'weekly', '{"ky":"kstk","val":10}', '{"kr":50}', 8, '', 53);
