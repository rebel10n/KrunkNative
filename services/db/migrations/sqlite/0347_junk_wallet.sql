-- 0347_junk_wallet (SQLite): same as postgres/0347; the Junk wallet (doc/v10/junk.md). The balance
-- is players.player_junk in hundredths, never negative; junk_transactions is its ledger.
-- Owner: services/brain/db_junk.c. Additive only.
ALTER TABLE players ADD COLUMN player_junk INTEGER NOT NULL DEFAULT 0 CHECK (player_junk >= 0);

CREATE TABLE junk_transactions (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    amount       INTEGER NOT NULL,
    source       TEXT NOT NULL,
    ref          TEXT,
    at           TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX junk_transactions_account_idx ON junk_transactions (account_id, at);
