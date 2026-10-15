-- 0210_creator_badge (SQLite): same as postgres/0210; Krunker v10.0.2's creator badge (the purple
-- "verified" icon, creator_badge / creatorBadge). Set by developers ("crt", services/brain/profiles.c).
ALTER TABLE players ADD COLUMN creator_badge INTEGER NOT NULL DEFAULT 0;
