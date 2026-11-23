-- 0399_krunkcup (PostgreSQL): Krunker v10.0.7's World KrunkCup 2026 behind kn_brain. The Leaderboards' KrunkCup view
-- (deob-10.0.7.js:140281-140298 markup, 140940-141145 logic) reads leaderboards.getKrunkCupTeams / getKrunkCupContributors
-- / getKrunkCupTopPlayers (27142-27158): a team is a style of the KrunkCup Jerseys (store 7794, config.krunkCupJersey
-- 19788; its 48 `styles`, from bundle #30) and its id the style index; each team row has total_score, total_kills and
-- total_games, a contributor player_name, player_clan, kills, score and games. Its empty text says how a player joins:
-- "Equip a KrunkCup jersey with a team style and play!". The KrunkCup Participation Band (store 7803,
-- config.krunkCupWristband 19789, the same 48 styles) is claimed through krunkcup.getWristbandStatus / claimWristband
-- (27428-27440).
--
-- krunkcup_contributions: what each account's committed matches added to a team, one row per (account, team). The
-- game server marks a match played in the jerseys (server/src/accounts.c "kcJersey"); kn_brain reads the style the
-- account set on them (item_styles, 0393; none = style 0) and adds the match's kills, score and games
-- (services/brain/db_krunkcup.c db_krunkcup_credit, from P_PROGRESS). A team's totals are the sums of its rows.
-- krunkcup_wristbands: the account's claimed Participation Band and the team (style) it was given in.
-- Additive only.
CREATE TABLE krunkcup_contributions (
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    team        integer NOT NULL CHECK (team >= 0),
    kills       bigint NOT NULL DEFAULT 0,
    score       bigint NOT NULL DEFAULT 0,
    games       bigint NOT NULL DEFAULT 0,
    updated_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, team)
);
CREATE INDEX krunkcup_contributions_team ON krunkcup_contributions (team, score DESC);

CREATE TABLE krunkcup_wristbands (
    account_id  integer NOT NULL PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    team        integer NOT NULL CHECK (team >= 0),
    claimed_at  timestamptz NOT NULL DEFAULT now()
);
