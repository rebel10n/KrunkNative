-- 0398_item_instances (SQLite): Krunker v10.0.7's item instances behind kn_brain. A spin's `instances` ({id, m}:
-- deob-10.0.7.js:224680-224697, :224785-224790) are owned copies whose m carries a wear (m.wear, 0 <= wear < 1: the
-- Flawless / Tested / Trashed / Wrecked line, :16478-16494, and the model's wearMap, :224753-224762) and a mint serial
-- (m.serial, the "#n" serialTag, :24618-24620). Box Office keeps them as skin_copies rows (0352), so a minted copy is
-- listed, traded, quick sold and crafted by its copy id as a painted one is (doc/v10/paint-copy.md):
--   skin_copies.wear / serial: the copy's wear and mint serial (NULL none). A copy now needs no paint: a minted copy
--   has none until it is painted, so the paint CHECK becomes "at most one paint" (the table is rebuilt: SQLite cannot
--   drop a CHECK). The sequence keeps going from where it was (copy ids never repeat).
--   market_listings.wear / serial, trade_copies.wear / serial: a listed / escrowed copy keeps them, as it keeps its
--   paint (0353), and gets them back with the copy.
--   item_mints: the serials minted per store item (the next serial is minted + 1; a consumed copy's serial is never
--   reused).
-- And the Paint Ticket (store index 7792) in Box Office's catalogue: krunker.io sells it for USD (0354 product 33,
-- io.krunker.paintticket.dyn.real, not served: Box Office has no payments); Box Office sells it for KR, so the Spin
-- Error's "Purchase More" (deob-10.0.7.js:224246-224268) finds a catalogue entry that rewards the ticket. 50 KR is the
-- USD price at Box Office's KR rate (the VIP pass: $0.99 a skipped level = 200 KR, 0108_seasons). Additive otherwise.
CREATE TABLE skin_copies_0398 (
    copy_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id      INTEGER NOT NULL CHECK (skin_id >= 0),
    paint_color  TEXT,                 -- '#rrggbb' for a colour paint, else NULL
    paint_shader INTEGER,              -- the shader id for a shader paint, else NULL
    painted_at   TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    wear         REAL CHECK (wear IS NULL OR (wear >= 0 AND wear < 1)), -- m.wear, NULL none
    serial       INTEGER CHECK (serial IS NULL OR serial > 0),         -- m.serial, NULL none
    CHECK (paint_color IS NULL OR paint_shader IS NULL)
);
INSERT INTO skin_copies_0398 (copy_id, account_id, skin_id, paint_color, paint_shader, painted_at)
    SELECT copy_id, account_id, skin_id, paint_color, paint_shader, painted_at FROM skin_copies;
DELETE FROM sqlite_sequence WHERE name = 'skin_copies_0398';
INSERT INTO sqlite_sequence (name, seq) SELECT 'skin_copies_0398', max(seq, 999999) FROM sqlite_sequence WHERE name = 'skin_copies';
INSERT INTO sqlite_sequence (name, seq) SELECT 'skin_copies_0398', 999999 WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = 'skin_copies_0398');
DROP TABLE skin_copies;
ALTER TABLE skin_copies_0398 RENAME TO skin_copies;
CREATE INDEX skin_copies_owner_idx ON skin_copies (account_id, skin_id);

ALTER TABLE market_listings ADD COLUMN wear REAL;
ALTER TABLE market_listings ADD COLUMN serial INTEGER;
ALTER TABLE trade_copies ADD COLUMN wear REAL;
ALTER TABLE trade_copies ADD COLUMN serial INTEGER;

CREATE TABLE item_mints (
    skin_id INTEGER PRIMARY KEY CHECK (skin_id >= 0),
    minted  INTEGER NOT NULL DEFAULT 0 CHECK (minted >= 0)
);

INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (90001, 'limited', 90001, 13, 'product', '{}', 'Paint Ticket', 'Spin the Paint Wheel for color and shader paints.', 'https://assets.krunker.io/textures/previews/cosmetics/13_0_18.png', 90001, 'kn.paintticket', 'virtual', NULL, 50, 1, 'KR', NULL, NULL, 90001, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (90001, 0, 90001, 'skin', '7792', 1);
