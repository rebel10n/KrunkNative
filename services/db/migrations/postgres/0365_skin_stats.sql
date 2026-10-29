-- 0365_skin_stats (PostgreSQL): Box Office counter skins (Krunker v10.0.4: a counter skin's copy counts a stat, its m[stat],
-- deob-10.0.4.js:172726-172735; the server's takeSkinStats adds the match's counts, :191561-191585). Box Office keeps the
-- count per store index: one row per (account, skin, stat), added to by the game server's commit (kn_commit_stats
-- "skinStats") and sent back in the owned list's m (db_owned_skins). Additive except kn_commit_stats, which
-- PostgreSQL runs as a function (db_accounts.c P_COMMIT_STATS) and so is redefined here with the skinStats rows.
CREATE TABLE skin_stats (
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     integer NOT NULL CHECK (skin_id >= 0),
    stat        text NOT NULL,
    count       bigint NOT NULL DEFAULT 0 CHECK (count >= 0),
    PRIMARY KEY (account_id, skin_id, stat)
);

CREATE OR REPLACE FUNCTION kn_commit_stats(p jsonb, p_max_kr integer) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_id     integer := (p ->> 'id')::integer;
    v_kills  integer := greatest(coalesce((p ->> 'kills')::integer, 0), 0);
    v_deaths integer := greatest(coalesce((p ->> 'deaths')::integer, 0), 0);
    v_score  integer := greatest(coalesce((p ->> 'score')::integer, 0), 0);
    v_base   bigint  := least(greatest(coalesce((p ->> 'xp')::bigint, (p ->> 'score')::bigint, 0), 0), 2147483647);
    v_comp   boolean := coalesce((p ->> 'comp')::numeric, 0) <> 0;
    v_ts     timestamp := clock_timestamp() AT TIME ZONE 'UTC';
    v_dcl    boolean;
    v_cls    bigint;
    v_won    boolean := coalesce((p ->> 'won')::numeric, 0) <> 0;
    v_games  integer := greatest(coalesce((p ->> 'games')::integer, 0), 0);
    v_time   bigint  := greatest(coalesce((p ->> 'time_ms')::bigint, 0), 0);
    v_kr     integer := least(greatest(coalesce((p ->> 'kr')::integer, 0), 0), greatest(p_max_kr, 0));
    v_class  integer := (p ->> 'class')::integer;
    v_stats  jsonb   := CASE WHEN jsonb_typeof(p -> 'stats') = 'object' THEN p -> 'stats' ELSE '{}'::jsonb END;
    v_match  text    := coalesce(nullif(p ->> 'match', ''), 'nomatch:' || gen_random_uuid()::text);
    v_clan   integer;
    v_r      jsonb;
    v_sk     numeric;
    v_cn     numeric;
BEGIN
    -- v10 isDoubleClassXP, fixed when the match started: the game server sends it per round ("classDouble");
    -- without it (an older server) the schedule at the commit: Saturday and Sunday (UTC) of an even ISO week
    v_dcl := CASE WHEN (p ->> 'classDouble') IS NOT NULL THEN (p ->> 'classDouble')::numeric <> 0
                  ELSE extract(dow FROM v_ts) IN (0, 6) AND extract(week FROM v_ts)::integer % 2 = 0 END;
    v_cls := least(v_base * (CASE WHEN v_dcl THEN 2 ELSE 1 END), 2147483647);
    IF v_id IS NULL OR NOT EXISTS (SELECT 1 FROM players WHERE player_id = v_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Account not found.';
    END IF;
    INSERT INTO match_results (match_id, account_id, server, region, map, mode, class_id, kills, deaths,
                               score, won, games, time_ms, kr, stats)
    VALUES (v_match, v_id, p ->> 'server', p ->> 'region', p ->> 'map', p ->> 'mode', v_class,
            v_kills, v_deaths, v_score, v_won, v_games, v_time, v_kr, v_stats)
    ON CONFLICT (match_id, account_id) DO NOTHING;
    IF NOT FOUND THEN
        RETURN kn_account_json(v_id) || jsonb_build_object('kr_awarded', 0, 'duplicate', 1);
    END IF;
    IF v_class IS NOT NULL AND v_class >= 0 THEN
        v_stats := v_stats || jsonb_build_object('c' || v_class, v_cls);
    END IF;
    UPDATE players SET
        player_kills        = player_kills + v_kills,
        player_deaths       = player_deaths + v_deaths,
        player_wins         = player_wins + CASE WHEN v_won THEN 1 ELSE 0 END,
        player_games_played = player_games_played + v_games,
        player_score        = player_score + v_base,
        player_timeplayed   = player_timeplayed + v_time,
        player_funds        = player_funds + v_kr,
        player_stats        = kn_jsonb_add(player_stats, v_stats),
        player_region       = coalesce((p ->> 'region_index')::integer, player_region),
        updated_at          = now()
    WHERE player_id = v_id;
    IF v_kr > 0 THEN
        INSERT INTO kr_transactions (account_id, amount, source, ref) VALUES (v_id, v_kr, 'reward', v_match);
    END IF;
    UPDATE clan_members SET score = score + v_base, kills = kills + v_kills, deaths = deaths + v_deaths
    WHERE account_id = v_id RETURNING clan_id INTO v_clan;
    IF v_clan IS NOT NULL THEN
        UPDATE clans SET clan_score = clan_score + v_base WHERE clan_id = v_clan;
        UPDATE players SET player_clankills = player_clankills + v_kills,
                           player_clandeaths = player_clandeaths + v_deaths WHERE player_id = v_id;
    END IF;
    -- v10.0.4 counter skins (db_accounts.c commit_skin_stats matches): the game server's takeSkinStats rows add to
    -- skin_stats, a known stat on a skin the account owns, 1..100000 per row, row by row so repeats add up
    FOR v_r IN SELECT value FROM jsonb_array_elements(
                   CASE WHEN jsonb_typeof(p -> 'skinStats') = 'array' THEN p -> 'skinStats' ELSE '[]'::jsonb END) LOOP
        CONTINUE WHEN jsonb_typeof(v_r) <> 'object' OR jsonb_typeof(v_r -> 'stat') IS DISTINCT FROM 'string'
            OR NOT (v_r ->> 'stat' = ANY (ARRAY['kills', 'hskills', 'aikills', 'headshots', 'hits', 'shots', 'jumps', 'candy']));
        v_sk := CASE WHEN jsonb_typeof(v_r -> 'skin') = 'number' THEN trunc((v_r ->> 'skin')::numeric) ELSE -1 END;
        v_cn := CASE WHEN jsonb_typeof(v_r -> 'count') = 'number' THEN trunc((v_r ->> 'count')::numeric) ELSE 0 END;
        CONTINUE WHEN v_sk < 0 OR v_sk > 2147483647 OR v_cn < 1 OR v_cn > 100000;
        CONTINUE WHEN NOT EXISTS (SELECT 1 FROM inventory WHERE account_id = v_id AND skin_id = v_sk::integer AND count > 0);
        INSERT INTO skin_stats (account_id, skin_id, stat, count) VALUES (v_id, v_sk::integer, v_r ->> 'stat', v_cn::bigint)
        ON CONFLICT (account_id, skin_id, stat) DO UPDATE SET count = skin_stats.count + EXCLUDED.count;
    END LOOP;
    -- v10 end-screen 2x XP offer: the match just played, when it counted (not a competitive game)
    INSERT INTO double_xp_offers (account_id, offered, games, xp)
    VALUES (v_id, CASE WHEN v_games > 0 AND NOT v_comp THEN 1 ELSE 0 END,
            (SELECT player_games_played FROM players WHERE player_id = v_id), v_base)
    ON CONFLICT (account_id) DO UPDATE SET offered = EXCLUDED.offered, games = EXCLUDED.games, xp = EXCLUDED.xp;
    RETURN kn_account_json(v_id) || jsonb_build_object('kr_awarded', v_kr, 'duplicate', 0);
END
$fn$;
