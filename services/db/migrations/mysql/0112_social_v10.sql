-- 0112_social_v10 (MySQL/MariaDB): same columns as postgres/0112_social_v10 (see there).
ALTER TABLE feed_posts ADD COLUMN funds BIGINT NOT NULL DEFAULT 0;
ALTER TABLE feed_posts ADD COLUMN ends_at DATETIME(3) NULL;
ALTER TABLE feed_posts ADD COLUMN settled_at DATETIME(3) NULL;
ALTER TABLE feed_posts ADD COLUMN winner_id BIGINT NULL;
CREATE INDEX feed_posts_giveaway_idx ON feed_posts (type, settled_at, ends_at);
