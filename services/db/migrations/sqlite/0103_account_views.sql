-- 0103_account_views (SQLite): the qs_player-shaped profile row of
-- postgres/0103 (player_rows). The account functions of postgres/0103 and
-- postgres/0104 are C (services/brain/db_*.c).

CREATE VIEW player_rows AS
SELECT p.player_id,
       a.username                                   AS player_name,
       p.player_alias,
       strftime('%Y-%m-%dT%H:%M:%SZ', a.created_at) AS player_datenew,
       p.player_kills, p.player_deaths, p.player_wins, p.player_games_played,
       p.player_score, p.player_timeplayed, p.player_funds,
       CAST(p.player_stats AS TEXT)                 AS player_stats,  -- a JSON *string*, like postgres' ::text
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
