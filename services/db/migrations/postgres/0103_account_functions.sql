-- 0103_account_functions: the account row/object builders and the multi-statement
-- account operations used by services/brain/accounts.c.  Owner: svc-accounts.
-- Functions raise SQLSTATE P0001 with a user-facing MESSAGE (shown in the client).

-- qs_player-shaped profile row (v3.1.1 get_qs_playerdata): what the social page's
-- profile / search / leaderboard rows read (t.player_name, t.player_stats as a
-- JSON *string*, t.player_datenew, t.player_clan, t.clan_rank ...).
CREATE VIEW player_rows AS
SELECT p.player_id,
       a.username                                   AS player_name,
       p.player_alias,
       to_char(a.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS player_datenew,
       p.player_kills, p.player_deaths, p.player_wins, p.player_games_played,
       p.player_score, p.player_timeplayed, p.player_funds,
       p.player_stats::text                         AS player_stats,
       p.player_lastreward, p.player_elo, p.player_elo2, p.player_elo4,
       p.player_chal, p.player_region, p.player_premium, p.player_type,
       p.player_developer, p.player_moderator, p.player_mapmod,
       p.player_featured, p.player_hack, p.player_infected, p.player_notrade,
       p.player_followed, p.player_following,
       p.partner_name, p.partner_approved, p.tw_twitchname,
       p.jobrating, p.jobratingpositive,
       c.clan_name                                  AS player_clan,
       coalesce(c.clan_rank, 0)                     AS clan_rank,
       coalesce(c.clan_region, 0)                   AS clan_regionid,
       coalesce(cm.kills, 0)                        AS clan_kills,
       coalesce(cm.deaths, 0)                       AS clan_deaths,
       cm.rank                                      AS clan_role,
       (a.twofa_secret IS NOT NULL)                 AS player_2fa
FROM players p
JOIN accounts a ON a.id = p.player_id
LEFT JOIN clan_members cm ON cm.account_id = p.player_id
LEFT JOIN clans c ON c.clan_id = cm.clan_id;

-- Active ban for an account and/or IP (newest permanent first), NULL row when none.
CREATE OR REPLACE FUNCTION kn_active_ban(p_account integer, p_ip text,
    OUT reason text, OUT expires_at timestamptz) RETURNS record
LANGUAGE sql STABLE AS $fn$
    SELECT b.reason, b.expires_at FROM bans b
    WHERE b.revoked_at IS NULL
      AND (b.expires_at IS NULL OR b.expires_at > now())
      AND ((p_account IS NOT NULL AND b.account_id = p_account) OR (p_ip IS NOT NULL AND b.ip = p_ip))
    ORDER BY b.expires_at DESC NULLS FIRST
    LIMIT 1
$fn$;

-- The account object handed to clients ("a" reply) and game servers (acct ops).
-- data = Account.getData() of src/data/account.js (40 slots, same order).
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

-- Login pre-check (v3.1.1 findByCredentialsWithIp): IP block, per (name, ip)
-- throttle, then the stored hash.  One row always.
CREATE OR REPLACE FUNCTION kn_login_lookup(p_name text, p_ip text, p_throttle_s integer,
    p_block_max integer, p_block_days integer)
RETURNS TABLE (ip_blocked boolean, throttled boolean, account_id integer, password_hash text,
               ban_reason text, ban_expires bigint, banned boolean)
LANGUAGE sql STABLE AS $fn$
    SELECT
        p_ip IS NOT NULL AND EXISTS (SELECT 1 FROM login_ip_blocks k
            WHERE k.ip = p_ip AND k.failures > p_block_max AND NOT k.whitelist
              AND k.updated_at > now() - make_interval(days => p_block_days)),
        p_ip IS NOT NULL AND p_throttle_s > 0 AND EXISTS (SELECT 1 FROM login_failures f
            WHERE lower(f.username) = lower(p_name) AND f.ip = p_ip
              AND f.at > now() - make_interval(secs => p_throttle_s)),
        a.id, a.password_hash,
        b.reason, coalesce(extract(epoch FROM b.expires_at)::bigint, 0),
        (b.reason IS NOT NULL OR b.expires_at IS NOT NULL)
    FROM (SELECT 1) one
    LEFT JOIN accounts a ON lower(a.username) = lower(p_name)
    LEFT JOIN LATERAL kn_active_ban(a.id, p_ip) b ON true
$fn$;

CREATE OR REPLACE FUNCTION kn_login_failed(p_name text, p_ip text) RETURNS void
LANGUAGE sql AS $fn$
    INSERT INTO login_failures (username, ip) SELECT p_name, p_ip WHERE p_ip IS NOT NULL;
    INSERT INTO login_ip_blocks (ip, failures) SELECT p_ip, 1 WHERE p_ip IS NOT NULL
        ON CONFLICT (ip) DO UPDATE SET failures = login_ip_blocks.failures + 1, updated_at = now();
$fn$;

-- Successful login/register/token resume: new session (p_token NULL = none),
-- audit row, optional password rehash.  Returns the account object.
CREATE OR REPLACE FUNCTION kn_login_success(p_id integer, p_ip text, p_via text,
    p_token text, p_ttl_s bigint, p_origin text, p_new_hash text) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
BEGIN
    IF p_token IS NOT NULL THEN
        INSERT INTO sessions (token_hash, account_id, expires_at, ip, origin)
        VALUES (sha256(convert_to(p_token, 'UTF8')), p_id, now() + make_interval(secs => p_ttl_s), p_ip, p_origin);
    END IF;
    INSERT INTO login_audit (account_id, ip, via) VALUES (p_id, p_ip, p_via);
    UPDATE accounts SET last_login_at = now(), last_login_ip = coalesce(p_ip, last_login_ip),
                        password_hash = coalesce(p_new_hash, password_hash)
    WHERE id = p_id;
    IF p_ip IS NOT NULL THEN
        DELETE FROM login_failures WHERE lower(username) = lower((SELECT username FROM accounts WHERE id = p_id)) AND ip = p_ip;
    END IF;
    RETURN kn_account_json(p_id);
END
$fn$;

CREATE OR REPLACE FUNCTION kn_register(p_name text, p_hash text, p_ip text, p_ref text,
    p_starting_kr bigint, p_premium integer, p_token text, p_ttl_s bigint, p_origin text) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_id integer;
BEGIN
    IF EXISTS (SELECT 1 FROM accounts WHERE lower(username) = lower(p_name)) THEN
        RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Username already taken.';
    END IF;
    INSERT INTO accounts (username, password_hash, created_ip, referral)
    VALUES (p_name, p_hash, p_ip, nullif(p_ref, '')) RETURNING id INTO v_id;
    INSERT INTO players (player_id, player_funds, player_premium) VALUES (v_id, greatest(p_starting_kr, 0), p_premium);
    IF p_starting_kr > 0 THEN
        INSERT INTO kr_transactions (account_id, amount, source) VALUES (v_id, p_starting_kr, 'starting');
    END IF;
    RETURN kn_login_success(v_id, p_ip, 'register', p_token, p_ttl_s, p_origin, NULL);
END
$fn$;

-- Session lookup (sliding expiry).  NULL when unknown/expired.
CREATE OR REPLACE FUNCTION kn_session_account(p_token text, p_ttl_s bigint) RETURNS integer
LANGUAGE sql AS $fn$
    UPDATE sessions SET last_seen_at = now(),
                        expires_at = greatest(expires_at, now() + make_interval(secs => p_ttl_s))
    WHERE token_hash = sha256(convert_to(p_token, 'UTF8')) AND expires_at > now()
    RETURNING account_id
$fn$;

-- Display name change ("nme").  Uniqueness against other display names and usernames.
CREATE OR REPLACE FUNCTION kn_set_alias(p_id integer, p_alias text) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_old text;
BEGIN
    SELECT player_alias INTO v_old FROM players WHERE player_id = p_id FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Account not found.';
    END IF;
    IF EXISTS (SELECT 1 FROM accounts WHERE lower(username) = lower(p_alias) AND id <> p_id)
       OR EXISTS (SELECT 1 FROM players WHERE lower(player_alias) = lower(p_alias) AND player_id <> p_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Name already taken.';
    END IF;
    UPDATE players SET player_alias = p_alias, updated_at = now() WHERE player_id = p_id;
    INSERT INTO name_changes (account_id, kind, old_name, new_name) VALUES (p_id, 'alias', v_old, p_alias);
    RETURN kn_account_json(p_id);
END
$fn$;

-- Match-end stats commit from a game server (op "stats"), idempotent per
-- (match, account).  p jsonb: {id, match, kills, deaths, score, won, games,
-- time_ms, kr, xp, class, mode, map, region, region_index, server, stats{}}.
CREATE OR REPLACE FUNCTION kn_commit_stats(p jsonb, p_max_kr integer) RETURNS jsonb
LANGUAGE plpgsql AS $fn$
DECLARE
    v_id     integer := (p ->> 'id')::integer;
    v_kills  integer := greatest(coalesce((p ->> 'kills')::integer, 0), 0);
    v_deaths integer := greatest(coalesce((p ->> 'deaths')::integer, 0), 0);
    v_score  integer := greatest(coalesce((p ->> 'score')::integer, 0), 0);
    v_xp     integer := greatest(coalesce((p ->> 'xp')::integer, (p ->> 'score')::integer, 0), 0);
    v_won    boolean := coalesce((p ->> 'won')::numeric, 0) <> 0;
    v_games  integer := greatest(coalesce((p ->> 'games')::integer, 0), 0);
    v_time   bigint  := greatest(coalesce((p ->> 'time_ms')::bigint, 0), 0);
    v_kr     integer := least(greatest(coalesce((p ->> 'kr')::integer, 0), 0), greatest(p_max_kr, 0));
    v_class  integer := (p ->> 'class')::integer;
    v_stats  jsonb   := CASE WHEN jsonb_typeof(p -> 'stats') = 'object' THEN p -> 'stats' ELSE '{}'::jsonb END;
    v_match  text    := coalesce(nullif(p ->> 'match', ''), 'nomatch:' || gen_random_uuid()::text);
    v_clan   integer;
BEGIN
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
        v_stats := v_stats || jsonb_build_object('c' || v_class, v_xp);
    END IF;
    UPDATE players SET
        player_kills        = player_kills + v_kills,
        player_deaths       = player_deaths + v_deaths,
        player_wins         = player_wins + CASE WHEN v_won THEN 1 ELSE 0 END,
        player_games_played = player_games_played + v_games,
        player_score        = player_score + v_xp,
        player_timeplayed   = player_timeplayed + v_time,
        player_funds        = player_funds + v_kr,
        player_stats        = kn_jsonb_add(player_stats, v_stats),
        player_region       = coalesce((p ->> 'region_index')::integer, player_region),
        updated_at          = now()
    WHERE player_id = v_id;
    IF v_kr > 0 THEN
        INSERT INTO kr_transactions (account_id, amount, source, ref) VALUES (v_id, v_kr, 'reward', v_match);
    END IF;
    UPDATE clan_members SET score = score + v_xp, kills = kills + v_kills, deaths = deaths + v_deaths
    WHERE account_id = v_id RETURNING clan_id INTO v_clan;
    IF v_clan IS NOT NULL THEN
        UPDATE clans SET clan_score = clan_score + v_xp WHERE clan_id = v_clan;
        UPDATE players SET player_clankills = player_clankills + v_kills,
                           player_clandeaths = player_clandeaths + v_deaths WHERE player_id = v_id;
    END IF;
    RETURN kn_account_json(v_id) || jsonb_build_object('kr_awarded', v_kr, 'duplicate', 0);
END
$fn$;

-- KR delta with ledger row; raises 'Not enough KR' when the balance would go negative.
CREATE OR REPLACE FUNCTION kn_add_funds(p_id integer, p_amount bigint, p_source text, p_ref text)
RETURNS bigint LANGUAGE plpgsql AS $fn$
DECLARE
    v_funds bigint;
BEGIN
    UPDATE players SET player_funds = player_funds + p_amount, updated_at = now()
    WHERE player_id = p_id AND player_funds + p_amount >= 0
    RETURNING player_funds INTO v_funds;
    IF NOT FOUND THEN
        IF EXISTS (SELECT 1 FROM players WHERE player_id = p_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Not enough KR';
        END IF;
        RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Account not found.';
    END IF;
    IF p_amount <> 0 THEN
        INSERT INTO kr_transactions (account_id, amount, source, ref) VALUES (p_id, p_amount, p_source, p_ref);
    END IF;
    RETURN v_funds;
END
$fn$;

-- Skin delta with audit row; raises when the player does not own enough.
CREATE OR REPLACE FUNCTION kn_add_skin(p_id integer, p_skin integer, p_delta integer, p_source text, p_ref text)
RETURNS integer LANGUAGE plpgsql AS $fn$
DECLARE
    v_count integer;
BEGIN
    IF p_delta >= 0 THEN
        INSERT INTO inventory (account_id, skin_id, count) VALUES (p_id, p_skin, p_delta)
        ON CONFLICT (account_id, skin_id) DO UPDATE SET count = inventory.count + p_delta
        RETURNING count INTO v_count;
    ELSE
        UPDATE inventory SET count = count + p_delta
        WHERE account_id = p_id AND skin_id = p_skin AND count + p_delta >= 0
        RETURNING count INTO v_count;
        IF NOT FOUND THEN
            RAISE EXCEPTION USING ERRCODE = 'P0001', MESSAGE = 'Item not owned';
        END IF;
    END IF;
    IF p_delta <> 0 THEN
        INSERT INTO skin_transactions (account_id, skin_id, delta, source, ref) VALUES (p_id, p_skin, p_delta, p_source, p_ref);
    END IF;
    RETURN v_count;
END
$fn$;
