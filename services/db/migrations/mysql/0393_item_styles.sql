-- 0393_item_styles (MySQL / MariaDB): same as postgres/0393. Krunker v10.0.7's item styles behind kn_brain
-- (db_economy.c run_item_style): one row per styled stack (copy_id 0) or painted copy of an account. Additive only.
CREATE TABLE item_styles (
    account_id  BIGINT NOT NULL,
    skin_id     INT NOT NULL,
    copy_id     BIGINT NOT NULL DEFAULT 0,
    style       INT NOT NULL CHECK (style > 0),
    styled_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, skin_id, copy_id),
    FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
