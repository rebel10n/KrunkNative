-- 0402_copy_name_tags (PostgreSQL): Krunker v10.0.7's name tags on item copies behind kn_brain. An item instance's
-- metadata m carries a name tag, m.name: the market card's "itemNametag" line (deob-10.0.7.js:110763-110767), a
-- listing's "listing-nametag" (:136680-136685), the profile listing's (:111978), the item page's instance overlay
-- "Name Tag" row (:109889-109894, openSales' data-nametag :223633-223640), the trade inventory's data-nametag tooltip
-- (:221822-221824, :223708) and the HUD's weapon slot "weapNametag" (:176440). Box Office keeps it on the copy:
--   skin_copies.name_tag: the copy's name tag (NULL none; 1..20 characters, set by an operator: v10.0.7's client has
--   no route that names a copy).
--   market_listings.name_tag, trade_copies.name_tag: a listed / escrowed copy keeps its name tag, as it keeps its
--   paint, wear and serial (0353, 0398), and the copy that comes back or goes to the buyer gets it again.
-- An item style (0393) does not move: the styled items are not tradable, sellable or quick-sellable. Additive only.
ALTER TABLE skin_copies ADD COLUMN name_tag text CHECK (name_tag IS NULL OR length(name_tag) BETWEEN 1 AND 20);
ALTER TABLE market_listings ADD COLUMN name_tag text;
ALTER TABLE trade_copies ADD COLUMN name_tag text;
