-- 0102_items_creations (MySQL/MariaDB): inventory/skins, market, trades, published
-- maps/mods/assets. Same tables as postgres/0102.

CREATE TABLE inventory (
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     BIGINT NOT NULL CHECK (skin_id >= 0),
    count       BIGINT NOT NULL DEFAULT 1 CHECK (count >= 0),
    first_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, skin_id)
);
CREATE INDEX inventory_skin_idx ON inventory (skin_id, count DESC);

CREATE TABLE skin_transactions (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     BIGINT NOT NULL,
    delta       BIGINT NOT NULL,
    source      VARCHAR(32) NOT NULL,
    ref         VARCHAR(128),
    at          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX skin_transactions_account_idx ON skin_transactions (account_id, at DESC);

CREATE TABLE market_listings (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    seller_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     BIGINT NOT NULL,
    price       BIGINT NOT NULL CHECK (price > 0),
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    buyer_id    BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    sold_at     DATETIME(3),
    cancelled_at DATETIME(3)
);
CREATE INDEX market_listings_open_idx   ON market_listings (skin_id, price);
CREATE INDEX market_listings_seller_idx ON market_listings (seller_id);
CREATE INDEX market_listings_sold_idx   ON market_listings (skin_id, sold_at DESC);

CREATE TABLE trades (
    id           BIGINT AUTO_INCREMENT PRIMARY KEY,
    sender_id    BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    receiver_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    message      TEXT NOT NULL DEFAULT (''),
    status       VARCHAR(32) NOT NULL DEFAULT 'open',
    created_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    closed_at    DATETIME(3),
    CHECK (sender_id <> receiver_id)
);
CREATE INDEX trades_receiver_idx ON trades (receiver_id, status);
CREATE INDEX trades_sender_idx   ON trades (sender_id, status);

CREATE TABLE trade_items (
    trade_id  BIGINT NOT NULL REFERENCES trades(id) ON DELETE CASCADE,
    side      VARCHAR(1) NOT NULL CHECK (side IN ('s', 'r')),
    skin_id   BIGINT NOT NULL,
    count     BIGINT NOT NULL DEFAULT 1 CHECK (count > 0),
    PRIMARY KEY (trade_id, side, skin_id)
);

CREATE TABLE published_maps (
    map_id            BIGINT AUTO_INCREMENT PRIMARY KEY,
    map_name          VARCHAR(128) NOT NULL,
    creator_id        BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    map_data          MEDIUMTEXT NOT NULL DEFAULT ('{}'),  -- map JSON, up to 500 kB
    map_description   TEXT NOT NULL DEFAULT (''),
    map_info          JSON NOT NULL DEFAULT ('{}'),
    map_image         MEDIUMTEXT,                   -- thumbnail data URL, up to 200 kB
    map_category      BIGINT NOT NULL DEFAULT 0,
    map_video         TEXT,
    map_featured      BIGINT NOT NULL DEFAULT 0,
    map_votes         BIGINT NOT NULL DEFAULT 0,
    map_plays         BIGINT NOT NULL DEFAULT 0,
    map_playtime      BIGINT NOT NULL DEFAULT 0,
    map_funds         BIGINT NOT NULL DEFAULT 0,
    map_updatecounter BIGINT NOT NULL DEFAULT 1,
    map_initialdate   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    map_date          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    deleted_at        DATETIME(3),
    live_name         VARCHAR(128) AS (CASE WHEN deleted_at IS NULL THEN map_name END) STORED
);
-- one live map per name (postgres: partial unique index on lower(map_name))
CREATE UNIQUE INDEX published_maps_name_lower_uq ON published_maps (live_name);
CREATE INDEX published_maps_creator_idx ON published_maps (creator_id);
CREATE INDEX published_maps_votes_idx ON published_maps (map_votes DESC);

CREATE TABLE published_mods (
    mod_id            BIGINT AUTO_INCREMENT PRIMARY KEY,
    mod_name          VARCHAR(128) NOT NULL,
    creator_id        BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    mod_url           TEXT NOT NULL DEFAULT (''),
    mod_description   TEXT NOT NULL DEFAULT (''),
    mod_info          JSON NOT NULL DEFAULT ('{}'),
    mod_image         MEDIUMTEXT,
    mod_video         TEXT,
    mod_featured      BIGINT NOT NULL DEFAULT 0,
    mod_votes         BIGINT NOT NULL DEFAULT 0,
    mod_plays         BIGINT NOT NULL DEFAULT 0,
    mod_updatecounter BIGINT NOT NULL DEFAULT 1,
    mod_initialdate   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    mod_date          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    deleted_at        DATETIME(3),
    live_name         VARCHAR(128) AS (CASE WHEN deleted_at IS NULL THEN mod_name END) STORED
);
CREATE UNIQUE INDEX published_mods_name_lower_uq ON published_mods (live_name);
CREATE INDEX published_mods_creator_idx ON published_mods (creator_id);

CREATE TABLE creation_votes (
    kind        BIGINT NOT NULL,
    creation_id BIGINT NOT NULL,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    vote        BIGINT NOT NULL DEFAULT 1,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (kind, creation_id, account_id)
);
CREATE TABLE creation_favs (
    kind        BIGINT NOT NULL,
    creation_id BIGINT NOT NULL,
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (kind, creation_id, account_id)
);

CREATE TABLE published_assets (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    creator_id  BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    name        VARCHAR(128) NOT NULL,
    category    BIGINT NOT NULL DEFAULT 0,
    data        JSON NOT NULL DEFAULT ('{}'),
    size_kb     BIGINT NOT NULL DEFAULT 0,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    deleted_at  DATETIME(3)
);
CREATE INDEX published_assets_creator_idx ON published_assets (creator_id);
