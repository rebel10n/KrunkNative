-- 0342_mail_kind (MySQL / MariaDB): v10's notification window types on the mail rows, as postgres/0342. kind 0 = a
-- Followers entry ("X has followed you!" with Follow Back; sender = the follower's name, written by social_eb.c
-- when a follow is not followed back yet), 3 = Inbox (every row before this). Additive: old rows read 3.
-- (Missing on MySQL until 2026-11-19: schema_parity_test.py.)
ALTER TABLE mail ADD COLUMN kind INT NOT NULL DEFAULT 3;
