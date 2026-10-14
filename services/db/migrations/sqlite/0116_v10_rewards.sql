-- 0116_v10_rewards (SQLite): same tables and columns as postgres/0116_v10_rewards (see there).
CREATE TABLE signup_rewards (
    account_id     INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    skins          TEXT NOT NULL DEFAULT '',
    premium_ms     INTEGER NOT NULL DEFAULT 0 CHECK (premium_ms >= 0),
    premium_until  TIMESTAMP,
    premium_swept  INTEGER NOT NULL DEFAULT 0,
    granted_at     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX signup_rewards_sweep_idx ON signup_rewards (premium_swept, premium_until);

CREATE TABLE signup_rewards_epoch (
    id     INTEGER PRIMARY KEY CHECK (id = 1),
    since  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
INSERT OR IGNORE INTO signup_rewards_epoch (id) VALUES (1);

ALTER TABLE battlepasses ADD COLUMN leveldata TEXT NOT NULL DEFAULT '';
ALTER TABLE battlepasses ADD COLUMN krdata TEXT NOT NULL DEFAULT '';
ALTER TABLE battlepasses ADD COLUMN jnkdata TEXT NOT NULL DEFAULT '';
