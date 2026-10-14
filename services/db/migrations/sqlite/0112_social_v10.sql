-- 0112_social_v10 (SQLite): same columns as postgres/0112_social_v10 (see there).
ALTER TABLE feed_posts ADD COLUMN funds INTEGER NOT NULL DEFAULT 0;
ALTER TABLE feed_posts ADD COLUMN ends_at TIMESTAMP;
ALTER TABLE feed_posts ADD COLUMN settled_at TIMESTAMP;
ALTER TABLE feed_posts ADD COLUMN winner_id INTEGER;
CREATE INDEX feed_posts_giveaway_idx ON feed_posts (ends_at) WHERE type = 9 AND settled_at IS NULL;
