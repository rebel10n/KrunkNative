-- 0111_economy (SQLite): same columns and tables as postgres/0111_economy (see there).
-- SQLite cannot add a constraint to an existing table, so counterparty_id is a plain integer
-- (kn_brain writes only existing account ids).

ALTER TABLE kr_transactions ADD COLUMN counterparty_id INTEGER;
ALTER TABLE kr_transactions ADD COLUMN fee INTEGER NOT NULL DEFAULT 0;

ALTER TABLE trades ADD COLUMN sender_kr   INTEGER NOT NULL DEFAULT 0 CHECK (sender_kr >= 0);
ALTER TABLE trades ADD COLUMN receiver_kr INTEGER NOT NULL DEFAULT 0 CHECK (receiver_kr >= 0);
ALTER TABLE trades ADD COLUMN expires_at  TIMESTAMP;
ALTER TABLE trades ADD COLUMN escrow      INTEGER NOT NULL DEFAULT 0;
CREATE INDEX trades_open_ttl_idx ON trades (expires_at);

CREATE TABLE economy_requests (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind       TEXT    NOT NULL,
    req_key    TEXT    NOT NULL CHECK (length(req_key) BETWEEN 1 AND 64),
    response   TEXT,
    at         TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, kind, req_key)
);
CREATE INDEX economy_requests_at_idx ON economy_requests (account_id, at);

CREATE TABLE feature_bans (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    feature     TEXT NOT NULL,
    reason      TEXT NOT NULL DEFAULT '',
    created_by  INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    expires_at  TIMESTAMP,
    revoked_at  TIMESTAMP
);
CREATE INDEX feature_bans_account_idx ON feature_bans (account_id, feature);
