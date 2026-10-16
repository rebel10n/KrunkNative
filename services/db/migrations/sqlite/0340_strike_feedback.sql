-- 0340_strike_feedback (SQLite): same as postgres/0340; Krunker Strike's feedback form (FRVR's
-- POST /v1/feedback). Owner: services/brain/db_strike.c. Additive only.
CREATE TABLE strike_feedback (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    message     TEXT NOT NULL,
    email       TEXT NOT NULL DEFAULT '',
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX strike_feedback_account_idx ON strike_feedback (account_id, created_at);
