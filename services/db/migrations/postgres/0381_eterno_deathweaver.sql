-- 0381_eterno_deathweaver (PostgreSQL): Krunker v10.0.5 "Added 1 new item to the Eterno Shop": the public "limited"
-- display (GET https://gapi.svc.krunker.io/shop/displays?tag=limited, v10's shop.getDisplayByTag, deob-10.0.7.js:26627-26631;
-- drawn by the Eterno Shop, :129566-129580, and read into store.catalog, :220848) gained product 47, Deathweaver (skin 8080,
-- USD 10, one per account) as its first node (id 49, order 0); every other node moved down one place. Read 2026-11-19
-- (display sha256 below); products, prices and limits otherwise match 0354. Additive: no row is deleted.
-- display sha256 eba4370029688f5757cfc027c4202264003222c86b70bbfe19d641842edd6605
INSERT INTO shop_official_products (id, display_tag, node_id, node_order, node_type, node_data, name, description, image_url, price_id, price_ref, price_type, price_usd, price_amount, price_currency_id, price_currency, purchase_limit, purchase_limit_scope, display_id, active) VALUES (47, 'limited', 49, 0, 'product', '{}', 'Deathweaver', 'Submachine Gun - Contraband', 'https://assets.krunker.io/textures/previews/weapons/weapon_4_m37_457.png', 48, 'io.krunker.skin8080', 'real', 10, NULL, NULL, NULL, 1, 'per_user', 49, 1);
INSERT INTO shop_official_contents (product_id, ord, content_id, content_type, ref, quantity) VALUES (47, 0, 46, 'skin', '8080', 1);
UPDATE shop_official_products SET node_order = 18 WHERE id = 4 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 20 WHERE id = 6 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 19 WHERE id = 11 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 21 WHERE id = 12 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 23 WHERE id = 15 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 29 WHERE id = 17 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 32 WHERE id = 21 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 31 WHERE id = 22 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 30 WHERE id = 23 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 15 WHERE id = 24 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 17 WHERE id = 25 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 13 WHERE id = 26 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 16 WHERE id = 27 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 14 WHERE id = 33 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 1 WHERE id = 35 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 2 WHERE id = 36 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 3 WHERE id = 37 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 4 WHERE id = 38 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 5 WHERE id = 39 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 6 WHERE id = 40 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 7 WHERE id = 41 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 8 WHERE id = 42 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 9 WHERE id = 43 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 10 WHERE id = 44 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 11 WHERE id = 45 AND display_tag = 'limited';
UPDATE shop_official_products SET node_order = 12 WHERE id = 46 AND display_tag = 'limited';
