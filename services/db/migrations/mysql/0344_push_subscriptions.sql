-- 0344_push_subscriptions (MySQL/MariaDB): Web Push subscriptions of the Krunker Box Office web client
-- (services/brain/push.c, db_push.c). Same table as postgres/0344: keyed by the endpoint (512 bytes at most,
-- services/brain/push_rules.h). p256dh / auth are base64url (87 and 22 characters). Additive only.
CREATE TABLE push_subscriptions (
    endpoint   VARCHAR(512) PRIMARY KEY,
    account_id BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    p256dh     VARCHAR(128) NOT NULL,
    auth       VARCHAR(64) NOT NULL,
    created_at BIGINT NOT NULL
);
CREATE INDEX push_subscriptions_account_idx ON push_subscriptions (account_id, created_at);
