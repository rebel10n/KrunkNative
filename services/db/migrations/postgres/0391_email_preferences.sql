-- 0391_email_preferences (PostgreSQL): Krunker v10.0.7's email preference (GET / POST /email/preferences,
-- deob-10.0.7.js:26484-26498) behind kn_brain (services/brain/v10_auth.c). Additive only.
--
-- accounts.email_pref_at: when the player last chose (NULL: never; GET answers chosen false and the client shows its
-- opt-in popup, deob-10.0.7.js:216865-216873). accounts.email_promotional: the choice (setPreference's promotional).
-- accounts.email_pref_source: where it was made ("popup", "registration" or "settings", 116865 / 117421 / 217697).
ALTER TABLE accounts ADD COLUMN email_pref_at timestamptz;
ALTER TABLE accounts ADD COLUMN email_promotional boolean NOT NULL DEFAULT false;
ALTER TABLE accounts ADD COLUMN email_pref_source text;
