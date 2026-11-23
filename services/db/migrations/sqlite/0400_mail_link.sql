-- 0400_mail_link (SQLite): a mail row's link and its v10 mail type, read by the notification window (windows[29])
-- as km_link / km_type (deob-10.0.7.js:221247-221253): a km_type 5 message's text links its "Post!" to
-- openPost(km_link). Box Office writes one: the giveaway winner's mail (db_social_v10.c). Additive: old rows read
-- NULL / 0.
ALTER TABLE mail ADD COLUMN link TEXT;
ALTER TABLE mail ADD COLUMN mail_type INTEGER NOT NULL DEFAULT 0;
