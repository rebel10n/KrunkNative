-- 0211_kpd_applications: K.P.D (Krunker Police Department) applications. v10.0.2's popup offers an
-- application from 20 positive reports (jobRatingPos) with a rating above 0.7; its kpdSubmit sends
-- "kpdA" [discord, reason] for a level 50 account. KrunkNative keeps them here (op kpd_apply,
-- services/brain/db_kpd.c); a developer reviews them in game (/kpd apps) and grants or revokes the
-- officer flag (/kpd <name> on|off: players.player_moderator, op kpd_officer), which also closes the
-- account's pending application. Owner: services/brain/db_kpd.c. Additive only.
CREATE TABLE kpd_applications (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    account_id  integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name        text NOT NULL DEFAULT '',
    discord     text NOT NULL DEFAULT '',
    reason      text NOT NULL DEFAULT '',
    status      integer NOT NULL DEFAULT 0,   -- 0 pending, 1 granted (the account became an officer)
    created_at  timestamptz NOT NULL DEFAULT now(),
    closed_at   timestamptz
);
CREATE INDEX kpd_applications_status_idx ON kpd_applications (status, created_at);
CREATE INDEX kpd_applications_account_idx ON kpd_applications (account_id, status);
