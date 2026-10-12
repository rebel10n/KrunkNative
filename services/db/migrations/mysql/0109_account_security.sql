-- 0109_account_security (MySQL/MariaDB): same columns and tables as postgres/0109_account_security (see there).

ALTER TABLE accounts ADD COLUMN email_verified_at DATETIME(3) NULL;
ALTER TABLE accounts ADD COLUMN twofa_pending VARCHAR(64) NULL;
ALTER TABLE accounts ADD COLUMN twofa_last_step BIGINT NOT NULL DEFAULT 0;
ALTER TABLE accounts ADD COLUMN twofa_failures INTEGER NOT NULL DEFAULT 0;
ALTER TABLE accounts ADD COLUMN twofa_failed_at DATETIME(3) NULL;

UPDATE accounts SET twofa_secret = NULL WHERE twofa_secret LIKE '$%';

CREATE TABLE email_tokens (
    token_hash  VARCHAR(64) PRIMARY KEY,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind        VARCHAR(16) NOT NULL DEFAULT 'verify',
    email       VARCHAR(254) NOT NULL,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    expires_at  DATETIME(3) NOT NULL,
    used_at     DATETIME(3) NULL
);
CREATE INDEX email_tokens_account_idx ON email_tokens (account_id, created_at);

CREATE TABLE mail_log (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT NULL REFERENCES accounts(id) ON DELETE SET NULL,
    kind        VARCHAR(16) NOT NULL,
    to_addr     VARCHAR(254) NOT NULL,
    ip          VARCHAR(64),
    status      VARCHAR(16) NOT NULL DEFAULT 'queued',
    message_id  VARCHAR(255),
    error       TEXT,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    sent_at     DATETIME(3) NULL
);
CREATE INDEX mail_log_account_idx ON mail_log (account_id, created_at);
CREATE INDEX mail_log_to_idx ON mail_log (to_addr, created_at);
CREATE INDEX mail_log_ip_idx ON mail_log (ip, created_at);
