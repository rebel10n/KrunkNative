-- 0371_ranked_shop_universal_ticket (SQLite): Krunker v10.0.7 "The Ranked Shop now sells the Universal Spin Ticket instead
-- of the Season 9 Heroic Ticket, and no longer sells the Season 9 Ultra Ticket" (krunker.io serves its shop from its API; no
-- game.js hunk). Box Office's rows are staff data (ranked_shop, 0205): a row selling the Season 9 Heroic Ticket (store
-- index 7509) sells the Universal Spin Ticket (7993, spinWheelId 2: store.wheels[2], the Heroic wheel; services/brain/store.c
-- ticket_wheel) at the same price and limits, and a live Season 9 Ultra Ticket row (7510) expires now. Rows are never
-- deleted: ranked_shop_purchases keeps pointing at them.
UPDATE ranked_shop SET skin_id = 7993 WHERE skin_id = 7509;
UPDATE ranked_shop SET expires_at = strftime('%Y-%m-%d %H:%M:%f', 'now') WHERE skin_id = 7510 AND (expires_at IS NULL OR expires_at > strftime('%Y-%m-%d %H:%M:%f', 'now'));
