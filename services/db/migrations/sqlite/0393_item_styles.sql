-- 0393_item_styles (SQLite): same as postgres/0393. Krunker v10.0.7's item styles behind kn_brain (db_economy.c
-- run_item_style): one row per styled stack (copy_id 0) or painted copy of an account. Additive only.
CREATE TABLE item_styles (
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     INTEGER NOT NULL,
    copy_id     INTEGER NOT NULL DEFAULT 0,
    style       INTEGER NOT NULL CHECK (style > 0),
    styled_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (account_id, skin_id, copy_id)
);
