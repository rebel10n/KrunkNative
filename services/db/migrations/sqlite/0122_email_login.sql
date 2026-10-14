-- 0122_email_login (SQLite): same index as postgres/0122_email_login (see there): an expression
-- index on lower(email), the expression kn_brain's email lookups use.

CREATE INDEX accounts_email_lower_idx ON accounts (lower(email));
