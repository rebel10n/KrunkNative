-- 0356_store_bundle_ownership (MySQL): the bundle grid's DATE and the account's bundle ownership (v10's
-- ALL_BUNDLE_DATA DATE and OWNED; db_shop.c). Additive only. Owner: services/brain/db_shop.c, shop.c.
ALTER TABLE store_bundles ADD COLUMN bundle_date VARCHAR(40) NULL;  -- DATE (ISO 8601), NULL: none (sorts last)

-- one row per bundle an account owns (v10's OWNED: the bundle id is in the account's list)
CREATE TABLE store_bundle_purchases (
    account_id BIGINT NOT NULL,
    bundle_id  BIGINT NOT NULL,
    at         DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, bundle_id),
    CONSTRAINT store_bundle_purchases_account FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE,
    CONSTRAINT store_bundle_purchases_bundle FOREIGN KEY (bundle_id) REFERENCES store_bundles(id) ON DELETE CASCADE
);
