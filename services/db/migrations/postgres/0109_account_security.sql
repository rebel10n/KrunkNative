-- 0109_account_security: Krunker v7.1.4 2FA and KrunkNative email verification. Owner: svc-accounts.
--
-- 2FA: v7.1.4 replaced v3.1.1's pattern grid with an authenticator app (index.js twoFAres /
-- act2FA / dact2FA / sub2FA, config.twoFAKeyLen 6, the QR code of an otpauth:// URI): TOTP
-- (RFC 6238, SHA-1, 30 s). accounts.twofa_secret (v7 player_2FA) now holds the base32 secret;
-- twofa_pending the secret offered by "a2FA" until "a2FAc" confirms a code; twofa_last_step the
-- last accepted time step (a code is never accepted twice); twofa_failures / twofa_failed_at
-- lock code guessing (10 wrong codes -> 15 minutes). v3.1.1 grid secrets (0105: a password hash
-- of the pattern) cannot answer v7's 6-digit prompt: they are switched off here.
--
-- Email: v7.1.4 has no email flow (Account.email, data[40], is never shown; its packets.json
-- reserves "cemail" / "emailv"). KrunkNative verifies the account's email with a link:
-- email_tokens holds sha256 of each emailed token; mail_log every mail (no credentials, no
-- tokens), which also drives the resend rate limits. doc/backend.md 7.9.

ALTER TABLE accounts ADD COLUMN email_verified_at timestamptz;
ALTER TABLE accounts ADD COLUMN twofa_pending text;
ALTER TABLE accounts ADD COLUMN twofa_last_step bigint NOT NULL DEFAULT 0;
ALTER TABLE accounts ADD COLUMN twofa_failures integer NOT NULL DEFAULT 0;
ALTER TABLE accounts ADD COLUMN twofa_failed_at timestamptz;

UPDATE accounts SET twofa_secret = NULL WHERE twofa_secret LIKE '$%';

CREATE TABLE email_tokens (
    token_hash  bytea PRIMARY KEY,                 -- sha256 of the emailed token ({hash:})
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind        text NOT NULL DEFAULT 'verify',
    email       text NOT NULL,                     -- the address the link proves
    created_at  timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz NOT NULL,
    used_at     timestamptz
);
CREATE INDEX email_tokens_account_idx ON email_tokens (account_id, created_at);

CREATE TABLE mail_log (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id  integer REFERENCES accounts(id) ON DELETE SET NULL,
    kind        text NOT NULL,                     -- 'verify'
    to_addr     text NOT NULL,
    ip          text,
    status      text NOT NULL DEFAULT 'queued',    -- queued | sent | failed
    message_id  text,
    error       text,                              -- the server's reply, never credentials
    created_at  timestamptz NOT NULL DEFAULT now(),
    sent_at     timestamptz
);
CREATE INDEX mail_log_account_idx ON mail_log (account_id, created_at);
CREATE INDEX mail_log_to_idx ON mail_log (to_addr, created_at);
CREATE INDEX mail_log_ip_idx ON mail_log (ip, created_at);
