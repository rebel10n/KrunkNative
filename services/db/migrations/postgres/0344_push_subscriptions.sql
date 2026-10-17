-- 0344_push_subscriptions (PostgreSQL): Web Push subscriptions of the Krunker Box Office web client
-- (services/brain/push.c, db_push.c). One row per browser subscription: the endpoint (PushSubscription.endpoint)
-- is the key, so a browser that signs in to another account moves its row there. p256dh / auth are the
-- subscription's keys (base64url), kept for the brain's send (RFC 8291 payload encryption; the send is a later
-- step). The brain keeps at most 5 rows per account (the oldest go). Additive only.
CREATE TABLE push_subscriptions (
    endpoint   text PRIMARY KEY,
    account_id integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    p256dh     text NOT NULL,
    auth       text NOT NULL,
    created_at bigint NOT NULL
);
CREATE INDEX push_subscriptions_account_idx ON push_subscriptions (account_id, created_at);
