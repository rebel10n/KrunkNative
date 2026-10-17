-- 0344_push_subscriptions (SQLite): Web Push subscriptions of the Krunker Box Office web client
-- (services/brain/push.c, db_push.c). One row per browser subscription, keyed by its endpoint; p256dh / auth
-- are the subscription's keys, kept for the brain's send (a later step). At most 5 rows per account. Additive only.
CREATE TABLE push_subscriptions (
    endpoint   TEXT PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    p256dh     TEXT NOT NULL,
    auth       TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX push_subscriptions_account_idx ON push_subscriptions (account_id, created_at);
