-- 0391_email_preferences (SQLite): same as postgres/0391. Krunker v10.0.7's email preference behind kn_brain
-- (services/brain/v10_auth.c): when it was chosen, the promotional choice and where it was made. Additive only.
ALTER TABLE accounts ADD COLUMN email_pref_at TIMESTAMP;
ALTER TABLE accounts ADD COLUMN email_promotional INTEGER NOT NULL DEFAULT 0;
ALTER TABLE accounts ADD COLUMN email_pref_source TEXT;
