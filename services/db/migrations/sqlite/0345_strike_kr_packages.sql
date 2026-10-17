-- 0345_strike_kr_packages (SQLite): Krunker Strike's KR packages, the store's Currency section (strike_products,
-- 0320). FRVR sells them for real money (price_type real_money, price_amount in US cents). KrunkNative has no
-- payment provider: with none configured (payments.provider), services/brain/strike_econ.c credits a pack at no
-- cost, the dev / test path. Additive only: the rows are inserted, nothing is changed.
INSERT INTO strike_products (id, section, title, price_type, price_currency, price_amount, rewards, refresh_s, max_purchases, sort) VALUES
    ('kr_500', 'currency', '500 KR', 'real_money', 'usd', 499, '[{"amount":500,"currency":"kr","type":"currency"}]', 0, 0, 6),
    ('kr_1500', 'currency', '1500 KR', 'real_money', 'usd', 1299, '[{"amount":1500,"currency":"kr","type":"currency"}]', 0, 0, 7);
