-- 0342_mail_kind (SQLite): v10's notification window types on the mail rows. kind 0 = a Followers entry
-- ("X has followed you!" with Follow Back; sender = the follower's name, written by social_eb.c when a
-- follow is not followed back yet), 3 = Inbox (every row before this). Additive: old rows read 3.
ALTER TABLE mail ADD COLUMN kind INTEGER NOT NULL DEFAULT 3;
