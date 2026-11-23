-- 0399_krunkcup (MySQL / MariaDB): same as postgres/0399. Krunker v10.0.7's World KrunkCup 2026 behind kn_brain: each
-- account's contributions to a team (a KrunkCup Jerseys style, store 7794) and its claimed Participation Band (store
-- 7803). services/brain/db_krunkcup.c. Additive only.
CREATE TABLE krunkcup_contributions (
    account_id  BIGINT NOT NULL,
    team        INT NOT NULL CHECK (team >= 0),
    kills       BIGINT NOT NULL DEFAULT 0,
    score       BIGINT NOT NULL DEFAULT 0,
    games       BIGINT NOT NULL DEFAULT 0,
    updated_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, team),
    KEY krunkcup_contributions_team (team, score),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);

CREATE TABLE krunkcup_wristbands (
    account_id  BIGINT NOT NULL PRIMARY KEY,
    team        INT NOT NULL CHECK (team >= 0),
    claimed_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
