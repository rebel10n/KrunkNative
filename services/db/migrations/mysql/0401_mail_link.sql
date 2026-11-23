-- 0401_mail_link (MySQL / MariaDB): a mail row's link and its v10 mail type, as postgres/0401.
ALTER TABLE mail ADD COLUMN link TEXT NULL;
ALTER TABLE mail ADD COLUMN mail_type INT NOT NULL DEFAULT 0;
