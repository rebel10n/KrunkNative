-- 0107_progression: player-viewer WP-E part 2c (doc/v7/player-viewer.md 1.4.2-1.4.4): Krunker
-- v7.1.4 badges (src/data/badges.js; v7's player_badge / qs_player_badgeallocation, procs 181),
-- data-driven challenges (src/data/challenges.js activeChallenges {CI, T, DAT, REW, PV, PT}; v7
-- procs 175 / 176) and per-account medal counts (game.incMedal, endData.mdls). Read and written
-- by kn_brain's C procs in services/brain/db_progress.c (every driver); the account object gains
-- "badges" (kn_account_json below, db_account_json in services/brain/db_common.c).
-- Owner: svc-accounts.

-- Earned badges (badges.js indices). The selected one stays players.player_stats.bdg (v7 stats.bdg).
CREATE TABLE player_badges (
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    badge       integer NOT NULL CHECK (badge >= 0),
    claimed_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, badge)
);

-- Challenge definitions: T (ctype) daily / weekly / map / weapon / event / nightmare / ranked /
-- season / clan, DAT and REW as v7's JSON texts, IC (icon: img/chals/icon_<n>.png), N (name; ''
-- = i18n chall.name.<ky>). Daily and weekly rows rotate: each account gets a few of them per
-- UTC day / week (db_progress.c); the others are active until completed.
CREATE TABLE challenges (
    challenge_id integer PRIMARY KEY CHECK (challenge_id > 0),
    ctype        text NOT NULL CHECK (length(ctype) BETWEEN 1 AND 16),
    dat          text NOT NULL,
    rew          text NOT NULL DEFAULT '{}',
    icon         integer NOT NULL DEFAULT 0,
    name         text NOT NULL DEFAULT '',
    active       smallint NOT NULL DEFAULT 1,
    sort         integer NOT NULL DEFAULT 0
);

-- Progress (PV) and completion (PT) per account, challenge and period (UTC day / ISO-like week
-- number for daily / weekly, 0 otherwise).
CREATE TABLE player_challenges (
    account_id   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    challenge_id integer NOT NULL REFERENCES challenges(challenge_id) ON DELETE CASCADE,
    period       integer NOT NULL DEFAULT 0,
    pv           bigint NOT NULL DEFAULT 0,
    pt           smallint NOT NULL DEFAULT 0,
    updated_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, challenge_id, period)
);

-- Medals earned over every committed match (overlay.medalsList names).
CREATE TABLE player_medals (
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    medal       text NOT NULL CHECK (length(medal) BETWEEN 1 AND 32),
    count       bigint NOT NULL DEFAULT 0 CHECK (count >= 0),
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

-- kn_account_json (0103) + "badges": the account's earned badge indices.
CREATE OR REPLACE FUNCTION kn_account_json(p_id integer) RETURNS jsonb
LANGUAGE sql STABLE AS $fn$
    SELECT jsonb_build_object(
        'id', r.player_id,
        'username', r.player_name,
        'alias', coalesce(nullif(btrim(r.player_alias), ''), r.player_name),
        'score', r.player_score,
        'funds', r.player_funds,
        'clan', r.player_clan,
        'clan_rank', r.clan_rank,
        'clan_role', r.clan_role,
        'premium', r.player_premium,
        'dev', r.player_developer,
        'mod', r.player_moderator,
        'hack', r.player_hack,
        'featured', r.player_featured,
        'elo', r.player_elo, 'elo2', r.player_elo2, 'elo4', r.player_elo4,
        'chal', r.player_chal,
        'banned', (b.reason IS NOT NULL OR b.expires_at IS NOT NULL),
        'ban_reason', b.reason,
        'ban_expires', coalesce(extract(epoch FROM b.expires_at)::bigint, 0),
        'badges', coalesce((SELECT jsonb_agg(pb.badge ORDER BY pb.badge) FROM player_badges pb
                            WHERE pb.account_id = r.player_id), '[]'::jsonb),
        'data', jsonb_build_array(
            r.player_id, r.player_kills, r.player_wins, r.player_games_played,
            r.player_deaths, r.player_funds, r.player_score, r.player_clan,
            r.player_timeplayed, r.player_featured,
            coalesce((SELECT jsonb_agg(jsonb_build_object('ind', i.skin_id, 'cnt', i.count) ORDER BY i.skin_id)
                      FROM inventory i WHERE i.account_id = r.player_id AND i.count > 0), '[]'::jsonb),
            r.player_lastreward, r.player_developer, r.player_following,
            r.player_followed, 0, r.player_stats::jsonb, r.player_hack,
            r.partner_name, r.partner_approved, r.player_region,
            r.player_moderator, r.player_elo, r.player_elo2, r.player_elo4,
            r.player_chal, r.player_infected, r.player_premium,
            CASE WHEN r.player_2fa THEN 1 ELSE 0 END,
            coalesce(nullif(btrim(r.player_alias), ''), r.player_name),
            r.player_datenew, r.tw_twitchname, r.player_mapmod,
            r.clan_deaths, r.clan_kills, NULL, r.clan_regionid, r.clan_rank,
            r.jobrating, r.jobratingpositive))
    FROM player_rows r
    LEFT JOIN LATERAL kn_active_ban(r.player_id, NULL) b ON true
    WHERE r.player_id = p_id
$fn$;
