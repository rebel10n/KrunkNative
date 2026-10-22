-- 0355_store_bundle_ownership (SQLite): the bundle grid's DATE and the account's bundle ownership (v10's
-- ALL_BUNDLE_DATA DATE and OWNED; db_shop.c). Additive only. Owner: services/brain/db_shop.c, shop.c.
ALTER TABLE store_bundles ADD COLUMN bundle_date TEXT;  -- DATE (ISO 8601), NULL: none (sorts last)

-- one row per bundle an account owns (v10's OWNED: the bundle id is in the account's list)
CREATE TABLE store_bundle_purchases (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    bundle_id  INTEGER NOT NULL REFERENCES store_bundles(id) ON DELETE CASCADE,
    at         TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, bundle_id)
);
