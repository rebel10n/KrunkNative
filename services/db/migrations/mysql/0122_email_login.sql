-- 0122_email_login (MySQL/MariaDB): same purpose as postgres/0122_email_login (see there). The
-- email column (TEXT) compares in the database's case-insensitive collation, so kn_brain's MySQL
-- lookups use `email = $1` and the index is on email itself (TEXT needs a prefix length: 191
-- characters keeps the key under 767 bytes in utf8mb4; longer addresses still match, the rows
-- are then re-checked). No expression index: MariaDB has none.

CREATE INDEX accounts_email_idx ON accounts (email(191));
