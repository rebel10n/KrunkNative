-- 0112_social_v10: Krunker v10.0.2 Social hub (services/brain/social_gapi.c): KR giveaway posts
-- (post type 9). funds = the prize the author paid (plus the 10 % fee, kept as a ledger row);
-- ends_at = when the draw happens; settled_at / winner_id = the draw's outcome (winner NULL with
-- settled_at set: nobody entered, the prize went back to the author). Additive: existing posts keep
-- funds 0 and no end.
ALTER TABLE feed_posts ADD COLUMN funds bigint NOT NULL DEFAULT 0;
ALTER TABLE feed_posts ADD COLUMN ends_at timestamptz;
ALTER TABLE feed_posts ADD COLUMN settled_at timestamptz;
ALTER TABLE feed_posts ADD COLUMN winner_id integer REFERENCES accounts(id) ON DELETE SET NULL;
CREATE INDEX feed_posts_giveaway_idx ON feed_posts (ends_at) WHERE type = 9 AND settled_at IS NULL;
