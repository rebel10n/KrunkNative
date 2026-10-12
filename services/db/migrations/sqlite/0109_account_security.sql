-- 0109_account_security (SQLite): same columns and tables as postgres/0109_account_security (see there).

ALTER TABLE accounts ADD COLUMN email_verified_at TIMESTAMP;
ALTER TABLE accounts ADD COLUMN twofa_pending TEXT;
ALTER TABLE accounts ADD COLUMN twofa_last_step INTEGER NOT NULL DEFAULT 0;
ALTER TABLE accounts ADD COLUMN twofa_failures INTEGER NOT NULL DEFAULT 0;
ALTER TABLE accounts ADD COLUMN twofa_failed_at TIMESTAMP;

UPDATE accounts SET twofa_secret = NULL WHERE twofa_secret LIKE '$%';

CREATE TABLE email_tokens (
    token_hash  TEXT PRIMARY KEY,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind        TEXT NOT NULL DEFAULT 'verify',
    email       TEXT NOT NULL,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    expires_at  TIMESTAMP NOT NULL,
    used_at     TIMESTAMP
);
CREATE INDEX email_tokens_account_idx ON email_tokens (account_id, created_at);

CREATE TABLE mail_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    kind        TEXT NOT NULL,
    to_addr     TEXT NOT NULL,
    ip          TEXT,
    status      TEXT NOT NULL DEFAULT 'queued',
    message_id  TEXT,
    error       TEXT,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    sent_at     TIMESTAMP
);
CREATE INDEX mail_log_account_idx ON mail_log (account_id, created_at);
CREATE INDEX mail_log_to_idx ON mail_log (to_addr, created_at);
CREATE INDEX mail_log_ip_idx ON mail_log (ip, created_at);
