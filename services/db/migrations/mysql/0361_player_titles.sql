-- 0361_player_titles (MySQL / MariaDB): the titles an account owns. Krunker v10.0.3 keeps them on the account
-- (`acc.titles`, setData slot 55, the profile's player_titles "87,90"; deob-10.0.3.js:128949, 129024) and its
-- Black Market purchase grants the item's `title` with the item (purchaseBlackMarketItem's reply carries
-- it, deob-10.0.3.js:152803-152806). Box Office grants titles only there (services/brain/db_economy.c
-- run_blackm_buy); "ttl" (social_data.c) and the gapi profile (social_gapi.c) read them. Additive only.
-- One row per (account, title): a second grant of the same title changes nothing. `source` says where it
-- came from ('blackmarket'), `ref` the store item that brought it.
CREATE TABLE player_titles (
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    title_id    INTEGER NOT NULL CHECK (title_id >= 0),
    source      VARCHAR(32) NOT NULL DEFAULT '',
    ref         BIGINT NOT NULL DEFAULT -1,
    granted_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, title_id)
);

-- An account that bought a title item before this table existed gets its title now (the blackM items'
-- `title` in assets/skins.json, the v10.0.3 store table).
INSERT INTO player_titles (account_id, title_id, source, ref)
SELECT DISTINCT account_id,
       CASE skin_id WHEN 1672 THEN 90 WHEN 1866 THEN 91 WHEN 2994 THEN 92 WHEN 2995 THEN 93 WHEN 2996 THEN 94
                    WHEN 2998 THEN 95 WHEN 3845 THEN 96 WHEN 3863 THEN 97 ELSE 87 END,
       'blackmarket', skin_id
FROM skin_transactions
WHERE source = 'blackmarket' AND skin_id IN (1672, 1866, 2994, 2995, 2996, 2998, 3845, 3863, 4531);
