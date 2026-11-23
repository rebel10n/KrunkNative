-- 0401_skin_maker (SQLite): same as postgres/0401; Krunker v10.0.7's skin makers (acc.isSkinMaker,
-- ~~player_skinmaker). Set by developers ("skm", services/brain/profiles.c).
ALTER TABLE players ADD COLUMN player_skinmaker INTEGER NOT NULL DEFAULT 0;
