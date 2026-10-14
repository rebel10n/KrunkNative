-- 0116_v10_rewards (MySQL/MariaDB): same tables and columns as postgres/0116_v10_rewards (see there).
CREATE TABLE signup_rewards (
    account_id     BIGINT PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    skins          TEXT NOT NULL DEFAULT (''),
    premium_ms     BIGINT NOT NULL DEFAULT 0 CHECK (premium_ms >= 0),
    premium_until  DATETIME(3) NULL,
    premium_swept  SMALLINT NOT NULL DEFAULT 0,
    granted_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX signup_rewards_sweep_idx ON signup_rewards (premium_swept, premium_until);

CREATE TABLE signup_rewards_epoch (
    id     SMALLINT PRIMARY KEY CHECK (id = 1),
    since  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
INSERT IGNORE INTO signup_rewards_epoch (id) VALUES (1);

ALTER TABLE battlepasses ADD COLUMN leveldata TEXT NOT NULL DEFAULT ('');
ALTER TABLE battlepasses ADD COLUMN krdata TEXT NOT NULL DEFAULT ('');
ALTER TABLE battlepasses ADD COLUMN jnkdata TEXT NOT NULL DEFAULT ('');
