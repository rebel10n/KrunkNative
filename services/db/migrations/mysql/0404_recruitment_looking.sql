-- 0404_recruitment_looking (MySQL / MariaDB): Looking For is the recruitment form's
-- maxlength (deob-10.0.7.js recruit-looking, 500). The column was VARCHAR(200).
ALTER TABLE clan_recruitment MODIFY looking_for VARCHAR(500) NOT NULL DEFAULT '';
