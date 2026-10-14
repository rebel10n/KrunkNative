-- 0111_economy (MySQL/MariaDB): same columns and tables as postgres/0111_economy (see there).

ALTER TABLE kr_transactions ADD COLUMN counterparty_id BIGINT NULL;
ALTER TABLE kr_transactions ADD COLUMN fee BIGINT NOT NULL DEFAULT 0;

ALTER TABLE trades ADD COLUMN sender_kr   BIGINT NOT NULL DEFAULT 0 CHECK (sender_kr >= 0);
ALTER TABLE trades ADD COLUMN receiver_kr BIGINT NOT NULL DEFAULT 0 CHECK (receiver_kr >= 0);
ALTER TABLE trades ADD COLUMN expires_at  DATETIME(3) NULL;
ALTER TABLE trades ADD COLUMN escrow      SMALLINT NOT NULL DEFAULT 0;
CREATE INDEX trades_open_ttl_idx ON trades (expires_at);

CREATE TABLE economy_requests (
    account_id BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind       VARCHAR(16) NOT NULL,
    req_key    VARCHAR(64) NOT NULL,
    response   TEXT,
    at         DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, kind, req_key)
);
CREATE INDEX economy_requests_at_idx ON economy_requests (account_id, at);

CREATE TABLE feature_bans (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    feature     VARCHAR(16) NOT NULL,
    reason      TEXT NOT NULL DEFAULT (''),
    created_by  BIGINT NULL,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    expires_at  DATETIME(3) NULL,
    revoked_at  DATETIME(3) NULL
);
CREATE INDEX feature_bans_account_idx ON feature_bans (account_id, feature);
