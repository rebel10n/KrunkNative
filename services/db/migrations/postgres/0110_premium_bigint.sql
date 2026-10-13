-- 0110_premium_bigint (redis-tier, found by kn_dbcopy on a copy of the production SQLite file):
-- players.player_premium is v7.1.4's premiumT, milliseconds of premium. Staff grants of ten
-- years (315360000000 ms) overflow integer; SQLite (INTEGER) and MySQL (BIGINT) already hold
-- 64 bits, so only PostgreSQL changes (no sqlite / mysql file for this version). player_rows
-- selects the column, so it is dropped and recreated unchanged (0103's definition).
DROP VIEW player_rows;
ALTER TABLE players ALTER COLUMN player_premium TYPE bigint;
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
