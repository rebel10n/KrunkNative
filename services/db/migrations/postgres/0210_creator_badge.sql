-- 0210_creator_badge: Krunker v10.0.2's creator badge, the purple "verified" icon a creator's
-- profile, menu box and scoreboard rows show (v10 creator_badge / account.creatorBadge, colour
-- #993299). Developers set it ("crt", services/brain/profiles.c, logged as creator / uncreator);
-- the profile (P_PROFILE) and the account info (kn_account_json, db_account_json in
-- services/brain/db_common.c) send it. player_rows is unchanged.
ALTER TABLE players ADD COLUMN creator_badge smallint NOT NULL DEFAULT 0;

-- kn_account_json (0107) + "creator_badge".
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
