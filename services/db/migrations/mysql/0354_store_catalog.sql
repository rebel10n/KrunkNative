-- 0354_store_catalog (MySQL / MariaDB): the Store's catalogue as Krunker v10.0.3 serves it, for the Box Office
-- Eterno Shop and the store home's bundle grid (doc/v10/ui-backend-audit.md rows 16-17). Owner: services/brain/db_shop.c,
-- shop.c. Additive only.
--
-- shop_official_products / shop_official_contents: the products of krunker.io's public "limited" display
-- (GET https://gapi.svc.krunker.io/shop/displays?tag=limited, v10's shop.getDisplayByTag; read 2026-10-22), one row
-- per product with its display node, verbatim: v10's product ids, names, descriptions, image URLs, prices (a real USD
-- price or a virtual KR one), contents (a skin, a currency or a badge), purchase limits and their scope, and the
-- dynamic node's quantity steps. The live display's purchase counts are krunker.io's and are not copied: Box Office
-- counts its own purchases (shop_official_purchases). The Junk-priced products of 0349 stay in shop_products.
--
-- store_bundles: the rows v10 receives as ALL_BUNDLE_DATA ("abd": ID, NAME, PRICE, ORIGINAL_PRICE, DISCOUNT_PCT, SKINS,
-- EXPIRY, INFO, FUNDS, JUNK, PREMIUM, EVENT_NAME, EVENT_ENDS_AT; deob-10.0.3.js 242853, the card 144980-145070).
-- krunker.io sends them over its game socket only (no public route, no static file carries them), so the table starts
-- empty and the grid shows what v10 shows without a bundle: nothing. Rows are published by staff.
CREATE TABLE shop_official_products (
    id                   BIGINT PRIMARY KEY,                 -- v10's product id, verbatim
    display_tag          TEXT NOT NULL DEFAULT 'limited',
    node_id              BIGINT NOT NULL,                    -- the display node (children[].id)
    node_order           BIGINT NOT NULL,                    -- children[].order
    node_type            TEXT NOT NULL DEFAULT 'product' CHECK (node_type IN ('product', 'dynamic')),
    node_data            TEXT NOT NULL DEFAULT '{}',      -- children[].data (JSON)
    name                 TEXT NOT NULL,
    description          TEXT NOT NULL DEFAULT '',
    image_url            TEXT NOT NULL DEFAULT '',
    price_id             BIGINT NOT NULL,                    -- prices[0].id
    price_ref            TEXT NOT NULL,                    -- prices[0].price_ref
    price_type           TEXT NOT NULL CHECK (price_type IN ('real', 'virtual')),
    price_usd            DOUBLE,                             -- real: prices[0].real.price_usd
    price_amount         BIGINT,                             -- virtual: prices[0].virtual.amount
    price_currency_id    BIGINT,                             -- virtual: 1 KR
    price_currency       TEXT,                            -- virtual: 'KR'
    purchase_limit       BIGINT,                             -- NULL: none
    purchase_limit_scope TEXT CHECK (purchase_limit_scope IS NULL OR purchase_limit_scope IN ('per_user', 'global')),
    display_id           BIGINT,                             -- display_ids[0] (NULL: the product carries none)
    active               BIGINT NOT NULL DEFAULT 1 CHECK (active IN (0, 1))
);
CREATE INDEX shop_official_products_tag_idx ON shop_official_products (display_tag, active, node_order);

CREATE TABLE shop_official_contents (
    product_id   BIGINT NOT NULL REFERENCES shop_official_products(id) ON DELETE CASCADE,
    ord          BIGINT NOT NULL,
    content_id   BIGINT NOT NULL,                            -- contents[].id
    content_type TEXT NOT NULL CHECK (content_type IN ('skin', 'currency', 'badge')),
    ref          TEXT NOT NULL,                           -- the skin index, the currency id or the badge id
    quantity     BIGINT NOT NULL CHECK (quantity > 0),
    PRIMARY KEY (product_id, ord)
);

-- one row per Box Office purchase of an official product (the limits count these, per account or globally)
CREATE TABLE shop_official_purchases (
    id           BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    product_id   BIGINT NOT NULL REFERENCES shop_official_products(id) ON DELETE CASCADE,
    quantity     BIGINT NOT NULL CHECK (quantity > 0),
    price_paid   BIGINT NOT NULL CHECK (price_paid >= 0),  -- KR
    at           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX shop_official_purchases_idx ON shop_official_purchases (product_id, account_id);

CREATE TABLE store_bundles (
    id             BIGINT PRIMARY KEY,                       -- v10's ID (the art is img/backgrounds/bundles/b_<ID>.png)
    name           TEXT NOT NULL,                         -- NAME
    price          DOUBLE NOT NULL,                          -- PRICE (USD)
    original_price DOUBLE,                                   -- ORIGINAL_PRICE
    discount_pct   DOUBLE,                                   -- DISCOUNT_PCT
    skins          TEXT NOT NULL DEFAULT '',              -- SKINS: comma-separated skin indices
    expiry         TEXT,                                  -- EXPIRY (ISO 8601), NULL for none
    info           TEXT NOT NULL DEFAULT '',              -- INFO
    funds          BIGINT NOT NULL DEFAULT 0,                -- FUNDS (KR)
    junk           DOUBLE NOT NULL DEFAULT 0,                -- JUNK
    premium        BIGINT NOT NULL DEFAULT 0,                -- PREMIUM (days)
    event_name     TEXT,                                  -- EVENT_NAME
    event_ends_at  TEXT,                                  -- EVENT_ENDS_AT (ISO 8601)
    active         BIGINT NOT NULL DEFAULT 1 CHECK (active IN (0, 1)),
    sort_order     BIGINT NOT NULL DEFAULT 0
);

-- the official "limited" display (id 4), verbatim
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (35, 'limited', 37, 0, 'product', '{}', 'Hovi', 'Spray - Uncommon', 'https://assets.krunker.io/textures/sprays/558.png', 36, 'io.krunker.skin7994', 'virtual', NULL, 100, 1, 'KR', 1, 'per_user', 37, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (35, 0, 34, 'skin', '7994', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (36, 'limited', 38, 1, 'product', '{}', 'Lain Player Card', 'Player Card - Uncommon', 'https://assets.krunker.io/textures/playercards/card_83.webp', 37, 'io.krunker.skin7995', 'virtual', NULL, 100, 1, 'KR', 1, 'per_user', 38, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (36, 0, 35, 'skin', '7995', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (37, 'limited', 39, 2, 'product', '{}', 'Lain Spray', 'Spray - Uncommon', 'https://assets.krunker.io/textures/sprays/559.png', 38, 'io.krunker.skin7996', 'virtual', NULL, 100, 1, 'KR', 1, 'per_user', 39, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (37, 0, 36, 'skin', '7996', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (38, 'limited', 40, 3, 'product', '{}', 'Hanami', 'Melee - Contraband', 'https://assets.krunker.io/textures/previews/melee/melee_206.png', 39, 'io.krunker.skin7997', 'real', 10, NULL, NULL, NULL, 1, 'per_user', 40, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (38, 0, 37, 'skin', '7997', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (39, 'limited', 41, 4, 'product', '{}', 'KT', 'Spray - Rare', 'https://assets.krunker.io/textures/sprays/560.png', 40, 'io.krunker.skin7998', 'virtual', NULL, 100, 1, 'KR', 1, 'per_user', 41, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (39, 0, 38, 'skin', '7998', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (40, 'limited', 42, 5, 'product', '{}', 'KPD', 'Spray - Rare', 'https://assets.krunker.io/textures/sprays/561.png', 41, 'io.krunker.skin7999', 'virtual', NULL, 100, 1, 'KR', 1, 'per_user', 42, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (40, 0, 39, 'skin', '7999', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (41, 'limited', 43, 6, 'product', '{}', 'Stonks!', 'Spray - Relic', 'https://assets.krunker.io/textures/sprays/562.png', 42, 'io.krunker.skin8000', 'virtual', NULL, 100, 1, 'KR', 1, 'per_user', 43, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (41, 0, 40, 'skin', '8000', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (42, 'limited', 44, 7, 'product', '{}', 'nL Player Card', 'Player Card - Uncommon', 'https://assets.krunker.io/textures/playercards/card_84.webp', 43, 'io.krunker.skin8001', 'virtual', NULL, 100, 1, 'KR', 1, 'per_user', 44, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (42, 0, 41, 'skin', '8001', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (43, 'limited', 45, 8, 'product', '{}', 'nL Spray', 'Spray - Uncommon', 'https://assets.krunker.io/textures/sprays/563.png', 44, 'io.krunker.skin8002', 'virtual', NULL, 100, 1, 'KR', 1, 'per_user', 45, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (43, 0, 42, 'skin', '8002', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (44, 'limited', 46, 9, 'product', '{}', 'Silver Paint', 'Consumable. Apply on a paintable item to plate it in reflective silver.', 'https://assets.krunker.io/textures/paints/paint_silver.webp', 45, 'io.krunker.skin7905', 'real', 20, NULL, NULL, NULL, NULL, NULL, 46, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (44, 0, 43, 'skin', '7905', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (45, 'limited', 47, 10, 'product', '{}', 'Eterno', 'Sniper Rifle - Contraband', 'https://assets.krunker.io/textures/previews/weapons/weapon_1_c9.png', 46, 'io.krunker.skin8003', 'virtual', NULL, 200000, 1, 'KR', 1, 'per_user', 47, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (45, 0, 44, 'skin', '8003', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (46, 'limited', 48, 11, 'product', '{}', 'Season 10 Contributor Badge', 'Badge - For everyone that helped Season 10. Thank you!', 'https://assets.krunker.io/textures/badges/badge_34.png', 47, 'io.krunker.badge67', 'real', 50, NULL, NULL, NULL, 1, 'per_user', 48, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (46, 0, 45, 'badge', '67', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (26, 'limited', 20, 12, 'product', '{}', 'Sandstorm OTs-14 Groza', 'Assault Rifle - Relic', 'https://assets.krunker.io/textures/previews/weapons/weapon_2_m33_550.png', 27, 'io.krunker.skin7718', 'real', 2.99, NULL, NULL, NULL, 1, 'per_user', 20, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (26, 0, 25, 'skin', '7718', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (33, 'limited', 33, 13, 'dynamic', '{"dynamic":{"min_quantity":10,"max_quantity":1000,"steps":[{"min_quantity":500,"price_usd":0.22},{"min_quantity":1000,"price_usd":0.2}]}}', 'Paint Ticket', 'Spin the Paint Wheel for color and shader paints. Buy in bulk and save.', 'https://assets.krunker.io/textures/previews/cosmetics/13_0_18.png', 34, 'io.krunker.paintticket.dyn.real', 'real', 0.25, NULL, NULL, NULL, NULL, NULL, NULL, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (33, 0, 32, 'skin', '7792', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (24, 'limited', 21, 14, 'product', '{}', 'Sand Rhino', 'Revolver - Relic', 'https://assets.krunker.io/textures/previews/weapons/weapon_5_m14_14.png', 25, 'io.krunker.skin7613', 'real', 2.99, NULL, NULL, NULL, 1, 'per_user', 21, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (24, 0, 23, 'skin', '7613', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (27, 'limited', 22, 15, 'product', '{}', 'Sandstorm Nagant M1895', 'Revolver - Relic', 'https://assets.krunker.io/textures/previews/weapons/weapon_5_m16_383.png', 28, 'io.krunker.skin7719', 'real', 1.99, NULL, NULL, NULL, 1, 'per_user', 22, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (27, 0, 26, 'skin', '7719', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (25, 'limited', 23, 16, 'product', '{}', 'Sand PP90', 'Akimbo Uzi - Relic', 'https://assets.krunker.io/textures/previews/weapons/weapon_10_m13_197.png', 26, 'io.krunker.skin7614', 'real', 1.99, NULL, NULL, NULL, 1, 'per_user', 23, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (25, 0, 24, 'skin', '7614', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (4, 'limited', 5, 17, 'product', '{}', 'Company Card', 'Company Card', 'https://assets.krunker.io/textures/previews/melee/melee_103_3.png', 4, 'io.krunker.skin7431', 'virtual', NULL, 5000, 1, 'KR', 1, 'per_user', 5, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (4, 0, 4, 'skin', '7431', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (11, 'limited', 12, 18, 'product', '{}', 'Selakuku', 'Limited Krunkuku', 'https://assets.krunker.io/textures/previews/cosmetics/12_18.png', 11, 'io.krunker.skin7438', 'real', 2.99, NULL, NULL, NULL, 1, 'per_user', 12, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (11, 0, 11, 'skin', '7438', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (6, 'limited', 7, 19, 'product', '{}', 'FRVR Mug', 'The worker''s mug', 'https://assets.krunker.io/textures/previews/melee/melee_153.png', 6, 'io.krunker.skin7433', 'virtual', NULL, 50, 1, 'KR', 1, 'per_user', 7, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (6, 0, 6, 'skin', '7433', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (12, 'limited', 13, 20, 'product', '{}', 'Eterno Mug', 'It fuels you', 'https://assets.krunker.io/textures/previews/melee/melee_153_1.png', 12, 'io.krunker.skin7439', 'virtual', NULL, 2000, 1, 'KR', 1, 'per_user', 13, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (12, 0, 12, 'skin', '7439', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (15, 'limited', 15, 22, 'product', '{}', 'Junk', 'Junk', '', 15, 'io.krunker.junk', 'virtual', NULL, 2000, 1, 'KR', 1000, 'global', 15, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (15, 0, 14, 'currency', '2', 100);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (17, 'limited', 29, 28, 'product', '{}', 'Clan Cape', 'Back - Relic', 'https://assets.krunker.io/textures/previews/cosmetics/2_297.png', 18, 'io.krunker.skin7578', 'virtual', NULL, 1000, 1, 'KR', 1, 'per_user', 29, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (17, 0, 17, 'skin', '7578', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (23, 'limited', 30, 29, 'product', '{}', 'Clan Tabard', 'Body - Relic', 'https://assets.krunker.io/textures/previews/cosmetics/2_296.png', 24, 'io.krunker.skin7581', 'virtual', NULL, 1000, 1, 'KR', 1, 'per_user', 30, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (23, 0, 22, 'skin', '7581', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (22, 'limited', 31, 30, 'product', '{}', 'Clan Banner', 'Melee - Relic', 'https://assets.krunker.io/textures/previews/melee/melee_157.png', 23, 'io.krunker.skin7580', 'virtual', NULL, 1000, 1, 'KR', 1, 'per_user', 31, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (22, 0, 21, 'skin', '7580', 1);
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (21, 'limited', 32, 31, 'product', '{}', 'Clan Pole Banner', 'Back - Relic', 'https://assets.krunker.io/textures/previews/cosmetics/2_298.png', 22, 'io.krunker.skin7579', 'virtual', NULL, 1000, 1, 'KR', 1, 'per_user', 32, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (21, 0, 20, 'skin', '7579', 1);
