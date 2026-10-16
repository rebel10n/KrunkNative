-- 0340_strike_feedback (MySQL/MariaDB): same as postgres/0340; Krunker Strike's feedback form
-- (FRVR's POST /v1/feedback). Owner: services/brain/db_strike.c. Additive only.
CREATE TABLE strike_feedback (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT NOT NULL,
    message     TEXT NOT NULL,
    email       VARCHAR(160) NOT NULL DEFAULT '',
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX strike_feedback_account_idx ON strike_feedback (account_id, created_at);
