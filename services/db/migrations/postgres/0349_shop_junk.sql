-- 0349_shop_junk (PostgreSQL): the Junk-priced shop (v10's shop purchase with currency_id 2, POST
-- shop/purchase; doc/v10/junk.md section 6). Owner: services/brain/db_shop.c, shop.c. Additive only.
-- A product is priced in Junk (hundredths per unit, spent through junk_spend, reason 'shop') and grants
-- its contents (skins) on purchase. v10's live "limited" display has no Junk-priced product at the time
-- of writing, so the table starts empty: rows are published by staff, never invented by the brain.
CREATE TABLE shop_products (
    id                 BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    display_tag        TEXT NOT NULL DEFAULT 'limited',
    name               TEXT NOT NULL,
    description        TEXT NOT NULL DEFAULT '',
    image_url          TEXT NOT NULL DEFAULT '',
    price_currency_id  BIGINT NOT NULL DEFAULT 2 CHECK (price_currency_id = 2),
    price_amount       BIGINT NOT NULL CHECK (price_amount > 0),
    purchase_limit     BIGINT CHECK (purchase_limit IS NULL OR purchase_limit > 0),
    active             BIGINT NOT NULL DEFAULT 1 CHECK (active IN (0, 1)),
    sort_order         BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX shop_products_tag_idx ON shop_products (display_tag, active, sort_order);

CREATE TABLE shop_product_contents (
    product_id   BIGINT NOT NULL REFERENCES shop_products(id) ON DELETE CASCADE,
    ord          BIGINT NOT NULL,
    content_type TEXT NOT NULL CHECK (content_type = 'skin'),
    skin_id      BIGINT NOT NULL,
    quantity     BIGINT NOT NULL CHECK (quantity > 0),
    PRIMARY KEY (product_id, ord)
);

-- one row per purchase (the purchase limit counts its quantities per account)
CREATE TABLE shop_purchases (
    id           BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id   BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    product_id   BIGINT NOT NULL REFERENCES shop_products(id) ON DELETE CASCADE,
    quantity     BIGINT NOT NULL CHECK (quantity > 0),
    price_paid   BIGINT NOT NULL CHECK (price_paid > 0),
    at           TIMESTAMP NOT NULL DEFAULT (now())
);
CREATE INDEX shop_purchases_account_idx ON shop_purchases (account_id, product_id);
