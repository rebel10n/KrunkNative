-- 0364_item_locks (MySQL): Box Office item locks (Krunker v10.0.4: item locks protect items from quick-sell, listing and
-- trades; krunker.io keeps them behind GET / POST items/locks, deob-10.0.4.js:108837-108862). One row per locked unit of an
-- account: a stack (copy_id 0: the item's unpainted units) or one painted copy (copy_id: its skin_copies id). The brain's
-- quick-sell, list and trade procs refuse a locked unit (services/brain/db_economy.c econ_locked). Additive only.
CREATE TABLE item_locks (
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     INT NOT NULL,
    copy_id     BIGINT NOT NULL DEFAULT 0,
    locked_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, skin_id, copy_id)
);
