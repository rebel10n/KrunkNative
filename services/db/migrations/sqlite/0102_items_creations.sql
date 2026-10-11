-- 0102_items_creations (SQLite): inventory/skins, market, trades, published
-- maps/mods/assets. Same tables as postgres/0102.

CREATE TABLE inventory (
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     INTEGER NOT NULL CHECK (skin_id >= 0),
    count       INTEGER NOT NULL DEFAULT 1 CHECK (count >= 0),
    first_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, skin_id)
);
CREATE INDEX inventory_skin_idx ON inventory (skin_id, count DESC);

CREATE TABLE skin_transactions (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     INTEGER NOT NULL,
    delta       INTEGER NOT NULL,
    source      TEXT NOT NULL,
    ref         TEXT,
    at          TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX skin_transactions_account_idx ON skin_transactions (account_id, at DESC);

CREATE TABLE market_listings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    seller_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     INTEGER NOT NULL,
    price       INTEGER NOT NULL CHECK (price > 0),
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    buyer_id    INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    sold_at     TIMESTAMP,
    cancelled_at TIMESTAMP
);
CREATE INDEX market_listings_open_idx   ON market_listings (skin_id, price) WHERE sold_at IS NULL AND cancelled_at IS NULL;
CREATE INDEX market_listings_seller_idx ON market_listings (seller_id) WHERE sold_at IS NULL AND cancelled_at IS NULL;
CREATE INDEX market_listings_sold_idx   ON market_listings (skin_id, sold_at DESC) WHERE sold_at IS NOT NULL;

CREATE TABLE trades (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id    INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    receiver_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    message      TEXT NOT NULL DEFAULT '',
    status       TEXT NOT NULL DEFAULT 'open',
    created_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    closed_at    TIMESTAMP,
    CHECK (sender_id <> receiver_id)
);
CREATE INDEX trades_receiver_idx ON trades (receiver_id, status);
CREATE INDEX trades_sender_idx   ON trades (sender_id, status);

CREATE TABLE trade_items (
    trade_id  INTEGER NOT NULL REFERENCES trades(id) ON DELETE CASCADE,
    side      TEXT NOT NULL CHECK (side IN ('s', 'r')),
    skin_id   INTEGER NOT NULL,
    count     INTEGER NOT NULL DEFAULT 1 CHECK (count > 0),
    PRIMARY KEY (trade_id, side, skin_id)
);

CREATE TABLE published_maps (
    map_id            INTEGER PRIMARY KEY AUTOINCREMENT,
    map_name          TEXT NOT NULL,
    creator_id        INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    map_data          TEXT NOT NULL DEFAULT '{}',
    map_description   TEXT NOT NULL DEFAULT '',
    map_info          JSON TEXT NOT NULL DEFAULT '{}',
    map_image         TEXT,
    map_category      INTEGER NOT NULL DEFAULT 0,
    map_video         TEXT,
    map_featured      INTEGER NOT NULL DEFAULT 0,
    map_votes         INTEGER NOT NULL DEFAULT 0,
    map_plays         INTEGER NOT NULL DEFAULT 0,
    map_playtime      INTEGER NOT NULL DEFAULT 0,
    map_funds         INTEGER NOT NULL DEFAULT 0,
    map_updatecounter INTEGER NOT NULL DEFAULT 1,
    map_initialdate   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    map_date          TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    deleted_at        TIMESTAMP
);
CREATE UNIQUE INDEX published_maps_name_lower_uq ON published_maps (lower(map_name)) WHERE deleted_at IS NULL;
CREATE INDEX published_maps_creator_idx ON published_maps (creator_id);
CREATE INDEX published_maps_votes_idx ON published_maps (map_votes DESC) WHERE deleted_at IS NULL;

CREATE TABLE published_mods (
    mod_id            INTEGER PRIMARY KEY AUTOINCREMENT,
    mod_name          TEXT NOT NULL,
    creator_id        INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    mod_url           TEXT NOT NULL DEFAULT '',
    mod_description   TEXT NOT NULL DEFAULT '',
    mod_info          JSON TEXT NOT NULL DEFAULT '{}',
    mod_image         TEXT,
    mod_video         TEXT,
    mod_featured      INTEGER NOT NULL DEFAULT 0,
    mod_votes         INTEGER NOT NULL DEFAULT 0,
    mod_plays         INTEGER NOT NULL DEFAULT 0,
    mod_updatecounter INTEGER NOT NULL DEFAULT 1,
    mod_initialdate   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    mod_date          TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    deleted_at        TIMESTAMP
);
CREATE UNIQUE INDEX published_mods_name_lower_uq ON published_mods (lower(mod_name)) WHERE deleted_at IS NULL;
CREATE INDEX published_mods_creator_idx ON published_mods (creator_id);

CREATE TABLE creation_votes (
    kind        INTEGER NOT NULL,
    creation_id INTEGER NOT NULL,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    vote        INTEGER NOT NULL DEFAULT 1,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (kind, creation_id, account_id)
);
CREATE TABLE creation_favs (
    kind        INTEGER NOT NULL,
    creation_id INTEGER NOT NULL,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (kind, creation_id, account_id)
);

CREATE TABLE published_assets (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    creator_id  INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
    name        TEXT NOT NULL,
    category    INTEGER NOT NULL DEFAULT 0,
    data        JSON TEXT NOT NULL DEFAULT '{}',
    size_kb     INTEGER NOT NULL DEFAULT 0,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    deleted_at  TIMESTAMP
);
CREATE INDEX published_assets_creator_idx ON published_assets (creator_id);
