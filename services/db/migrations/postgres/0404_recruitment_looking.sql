-- 0404_recruitment_looking (PostgreSQL): looking_for is already text. The cap is 500
-- characters, the recruitment form's maxlength (deob-10.0.7.js recruit-looking).
COMMENT ON COLUMN clan_recruitment.looking_for IS 'at most 500 characters';
