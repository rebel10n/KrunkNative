-- 0330_studios (SQLite): same tables as postgres/0330; Krunker v10.0.2's Studios (services/brain/db_studios.c).
CREATE TABLE studios (
    id                   INTEGER PRIMARY KEY AUTOINCREMENT,
    name                 TEXT NOT NULL,
    owner_id             INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    monetization         INTEGER NOT NULL DEFAULT 0,
    monetization_blocked INTEGER NOT NULL DEFAULT 0,
    created_at           TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE UNIQUE INDEX studios_name_lower_uq ON studios (lower(name));
CREATE INDEX studios_owner_idx ON studios (owner_id);

CREATE TABLE studio_members (
    studio_id  INTEGER NOT NULL REFERENCES studios(id) ON DELETE CASCADE,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    role       INTEGER NOT NULL DEFAULT 2,
    joined_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (studio_id, account_id)
);
CREATE INDEX studio_members_account_idx ON studio_members (account_id);

CREATE TABLE studio_invites (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    studio_id  INTEGER NOT NULL REFERENCES studios(id) ON DELETE CASCADE,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    invited_by INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    created_at TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    expires_at TIMESTAMP NOT NULL
);
CREATE UNIQUE INDEX studio_invites_uq ON studio_invites (studio_id, account_id);
CREATE INDEX studio_invites_account_idx ON studio_invites (account_id, expires_at);

CREATE TABLE studio_maps (
    map_id     INTEGER PRIMARY KEY REFERENCES published_maps(map_id) ON DELETE CASCADE,
    studio_id  INTEGER NOT NULL REFERENCES studios(id) ON DELETE CASCADE,
    added_by   INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    added_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX studio_maps_studio_idx ON studio_maps (studio_id);

CREATE TABLE studio_currencies (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    studio_id  INTEGER NOT NULL REFERENCES studios(id) ON DELETE CASCADE,
    name       TEXT NOT NULL,
    symbol     TEXT NOT NULL,
    decimals   INTEGER NOT NULL DEFAULT 0,
    active     INTEGER NOT NULL DEFAULT 1,
    created_at TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE UNIQUE INDEX studio_currencies_symbol_uq ON studio_currencies (studio_id, symbol);

CREATE TABLE studio_monetization (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    studio_id    INTEGER NOT NULL REFERENCES studios(id) ON DELETE CASCADE,
    applicant_id INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    description  TEXT NOT NULL DEFAULT '',
    map_ids      TEXT NOT NULL DEFAULT '[]',
    status       TEXT NOT NULL DEFAULT 'pending',
    review_note  TEXT,
    reviewed_by  INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    reviewed_at  TIMESTAMP,
    created_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX studio_monetization_studio_idx ON studio_monetization (studio_id, created_at);
