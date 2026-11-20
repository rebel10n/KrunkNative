-- 0341_double_xp (MySQL / MariaDB): Krunker v10.0.2's double XP, as the brain applies it (services/brain/double_xp.h,
-- db_double_xp.c, double_xp_social.c), the tables of postgres/0341 and sqlite/0341. The end-screen 2x XP of the
-- last match per account (double_xp_offers: offered, games, xp, claimed_games = v10's lastDoubleAttempt) and
-- every claim (double_xp_claims: the provider and the ad proof, NULL for the placeholder). The class and
-- raid-drop doubling follow v10's weekend schedules and store nothing; the account JSON is built in C, as on
-- SQLite. Additive only. (Missing on MySQL until 2026-11-19: schema_parity_test.py.)
CREATE TABLE double_xp_offers (
    account_id    BIGINT PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    offered       SMALLINT NOT NULL DEFAULT 0,
    games         INT NOT NULL DEFAULT 0,
    xp            BIGINT NOT NULL DEFAULT 0,
    claimed_games INT NOT NULL DEFAULT 0
);

CREATE TABLE double_xp_claims (
    id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    games      INT NOT NULL,
    xp         BIGINT NOT NULL,
    provider   TEXT NOT NULL,
    ad_ref     TEXT,
    claimed_at BIGINT NOT NULL
);
CREATE INDEX double_xp_claims_account_idx ON double_xp_claims (account_id, games);
