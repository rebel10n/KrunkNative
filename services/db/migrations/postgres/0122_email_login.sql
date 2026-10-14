-- 0122_email_login: sign in with an email. Krunker v10.0.2's login modal takes an Email or a
-- Username ("(use username instead)"), so kn_brain's login (accounts.c SQL_LOGIN_EMAIL) and its
-- register / "cemail" checks look accounts up by lower(email); this index serves them.
-- Additive and forward-only: one index, no data rewritten, safe on the live database (the file's
-- transaction holds a SHARE lock on accounts while the index builds: writes wait, reads go on).
--
-- Not UNIQUE on purpose: until now an address could sit on several accounts (0109's password
-- reset mails up to 3 accounts per address), so a unique index could fail on existing data.
-- kn_brain keeps one account per address from here on instead: register (P_REGISTER_PRECHECK)
-- and "cemail" (P_EMAIL_REQUEST) refuse an email another account has ("Email already in use"),
-- and an email that older rows still share signs in to the verified holder, else the oldest.
-- Owner: svc-accounts.

CREATE INDEX accounts_email_lower_idx ON accounts (lower(email));
