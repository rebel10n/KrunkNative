-- 0402_copy_name_tags (MySQL / MariaDB): same as postgres/0402. Krunker v10.0.7's name tags on item copies behind
-- kn_brain: skin_copies.name_tag (the copy's m.name, NULL none, 1..20 characters, set by an operator), and the name
-- tag a listed / escrowed copy keeps (market_listings, trade_copies) until it comes back or moves on. Additive only.
ALTER TABLE skin_copies ADD COLUMN name_tag VARCHAR(80) NULL CHECK (name_tag IS NULL OR CHAR_LENGTH(name_tag) BETWEEN 1 AND 20);
ALTER TABLE market_listings ADD COLUMN name_tag VARCHAR(80) NULL;
ALTER TABLE trade_copies ADD COLUMN name_tag VARCHAR(80) NULL;
