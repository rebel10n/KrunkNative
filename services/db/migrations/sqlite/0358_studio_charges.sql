-- 0358_studio_charges (SQLite): same as postgres/0358: the KR monetized studios' games charge their players after
-- the consent ("chreq" / "chrsp", db_studios.c P_STUDIO_CHARGE): the gross KR paid and the studio's 90 % share, one row
-- per charge, req_key unique (a resent charge never takes the KR twice). Additive.
CREATE TABLE studio_charges (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    req_key     TEXT NOT NULL UNIQUE,
    studio_id   INTEGER NOT NULL REFERENCES studios(id) ON DELETE CASCADE,
    map_id      INTEGER NOT NULL,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    amount      INTEGER NOT NULL CHECK (amount > 0),
    earned      INTEGER NOT NULL CHECK (earned >= 0),
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX studio_charges_studio_idx ON studio_charges (studio_id, created_at);
CREATE INDEX studio_charges_account_idx ON studio_charges (account_id, created_at);
