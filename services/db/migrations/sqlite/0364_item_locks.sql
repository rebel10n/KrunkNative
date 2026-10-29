-- 0364_item_locks (SQLite): Box Office item locks (Krunker v10.0.4: item locks protect items from quick-sell, listing and
-- trades; krunker.io keeps them behind GET / POST items/locks, deob-10.0.4.js:108837-108862). One row per locked unit of an
-- account: a stack (copy_id 0: the item's unpainted units) or one painted copy (copy_id: its skin_copies id). The brain's
-- quick-sell, list and trade procs refuse a locked unit (services/brain/db_economy.c econ_locked). Additive only.
CREATE TABLE item_locks (
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     INTEGER NOT NULL,
    copy_id     INTEGER NOT NULL DEFAULT 0,
    locked_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, skin_id, copy_id)
);
