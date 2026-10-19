-- 0347_junk_wallet (PostgreSQL): the Junk wallet (doc/v10/junk.md). players.player_junk is the
-- balance in hundredths (12.34 Junk = 1234), never negative. junk_transactions is the ledger: one
-- row per credit or spend (signed hundredths, the reason as source, an optional ref), written in
-- the same transaction as the balance. Owner: services/brain/db_junk.c. Additive only.
ALTER TABLE players ADD COLUMN player_junk bigint NOT NULL DEFAULT 0;
ALTER TABLE players ADD CONSTRAINT players_junk_nonneg CHECK (player_junk >= 0);

CREATE TABLE junk_transactions (
    id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id   integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    amount       bigint NOT NULL,             -- signed delta, hundredths
    source       text NOT NULL,               -- 'trigger' | 'airdrop' | 'battlepass' | 'quicksell' | 'craft' | ...
    ref          text,
    at           timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX junk_transactions_account_idx ON junk_transactions (account_id, at DESC);

-- kn_account_json (0341) + "junk": the balance as text, "12.34" (the C db_account_json does the same).
-- The body is 0341's, unchanged apart from the one key.
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
        'creator_badge', (SELECT p.creator_badge FROM players p WHERE p.player_id = r.player_id),
        'junk', (SELECT (p.player_junk / 100)::text || '.' || lpad((p.player_junk % 100)::text, 2, '0')
                 FROM players p WHERE p.player_id = r.player_id),
        'elo', r.player_elo, 'elo2', r.player_elo2, 'elo4', r.player_elo4,
        'chal', r.player_chal,
        'banned', (b.reason IS NOT NULL OR b.expires_at IS NOT NULL),
        'ban_reason', b.reason,
        'ban_expires', coalesce(extract(epoch FROM b.expires_at)::bigint, 0),
        'badges', coalesce((SELECT jsonb_agg(pb.badge ORDER BY pb.badge) FROM player_badges pb
                            WHERE pb.account_id = r.player_id), '[]'::jsonb),
        'dxp', kn_dxp(r.player_id),
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
