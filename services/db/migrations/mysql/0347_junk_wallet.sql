-- 0347_junk_wallet (MySQL/MariaDB): same as postgres/0347; the Junk wallet (doc/v10/junk.md). The
-- balance is players.player_junk in hundredths, never negative; junk_transactions is its ledger.
-- Owner: services/brain/db_junk.c. Additive only.
ALTER TABLE players ADD COLUMN player_junk BIGINT NOT NULL DEFAULT 0 CHECK (player_junk >= 0);

CREATE TABLE junk_transactions (
    id           BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    amount       BIGINT NOT NULL,
    source       VARCHAR(32) NOT NULL,
    ref          VARCHAR(128),
    at           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX junk_transactions_account_idx ON junk_transactions (account_id, at);
