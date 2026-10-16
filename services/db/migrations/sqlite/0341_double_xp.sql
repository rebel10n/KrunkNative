-- 0341_double_xp (SQLite): Krunker v10.0.2's double XP, as the brain applies it (services/brain/double_xp.h,
-- db_double_xp.c, double_xp_social.c). The end-screen 2x XP of the last match per account
-- (double_xp_offers: offered, games, xp, claimed_games = v10's lastDoubleAttempt) and every claim
-- (double_xp_claims: the provider and the ad proof, NULL for the placeholder). The class and raid-drop
-- doubling follow v10's weekend schedules and store nothing. Additive only.
CREATE TABLE double_xp_offers (
    account_id    INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    offered       INTEGER NOT NULL DEFAULT 0,
    games         INTEGER NOT NULL DEFAULT 0,
    xp            INTEGER NOT NULL DEFAULT 0,
    claimed_games INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE double_xp_claims (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    games      INTEGER NOT NULL,
    xp         INTEGER NOT NULL,
    provider   TEXT NOT NULL,
    ad_ref     TEXT,
    claimed_at INTEGER NOT NULL
);
CREATE INDEX double_xp_claims_account_idx ON double_xp_claims (account_id, games);
