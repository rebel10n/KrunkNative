-- 0401_skin_maker: Krunker v10.0.7's skin makers (acc.isSkinMaker = ~~player_skinmaker, deob-10.0.7.js:115136).
-- getSkins gives a skin maker every season 6+ store item no creator made that is not blocked (:217781-217789): the
-- client's loadout lists them and the game server lets the account wear them (accounts_owns_skin). Developers set it
-- ("skm", services/brain/profiles.c, logged as skinmaker / unskinmaker); the account info (db_account_json in
-- services/brain/db_common.c, for every driver) sends it as player_skinmaker. kn_account_json and player_rows are
-- unchanged.
ALTER TABLE players ADD COLUMN player_skinmaker smallint NOT NULL DEFAULT 0;
