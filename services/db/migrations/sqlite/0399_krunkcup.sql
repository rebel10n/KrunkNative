-- 0399_krunkcup (SQLite): same as postgres/0399. Krunker v10.0.7's World KrunkCup 2026 behind kn_brain: each account's
-- contributions to a team (a KrunkCup Jerseys style, store 7794) and its claimed Participation Band (store 7803).
-- services/brain/db_krunkcup.c. Additive only.
CREATE TABLE krunkcup_contributions (
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    team        INTEGER NOT NULL CHECK (team >= 0),
    kills       INTEGER NOT NULL DEFAULT 0,
    score       INTEGER NOT NULL DEFAULT 0,
    games       INTEGER NOT NULL DEFAULT 0,
    updated_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, team)
);
CREATE INDEX krunkcup_contributions_team ON krunkcup_contributions (team, score DESC);

CREATE TABLE krunkcup_wristbands (
    account_id  INTEGER NOT NULL PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    team        INTEGER NOT NULL CHECK (team >= 0),
    claimed_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
