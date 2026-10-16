-- 0332_clan_logos: clan logos (services/brain/clan_logo.c, the picture pipeline of avatar.c). A
-- Captain or the owner uploads a 512x512 PNG (max 256 KB); kn_brain decodes it, re-encodes it as a
-- lossless WebP, has it reviewed by the same AI moderation model as profile pictures. Only an
-- approval charges 20,000 KR from the clan bank (no fee; ledger row source 'clan-logo'); a rejected
-- or failed review charges nothing, as a profile picture costs nothing. One row per clan:
-- the latest upload's review (status, version, sha256, reason, categories, model). The clans row
-- keeps v10's version numbers: clans.clan_logo_pending is the version under review (0 = none) and
-- clans.clan_logo the approved one (0 = none). image is the approved logo's WebP: it is written only
-- with an approval, and it stays shown while a newer upload is under review, so a picture that did
-- not pass is never stored. Additive: one new table. Owner: svc-accounts (services/brain).
CREATE TABLE clan_logos (
    clan_id       integer PRIMARY KEY REFERENCES clans(clan_id) ON DELETE CASCADE,
    status        text NOT NULL CHECK (status IN ('pending', 'approved', 'rejected', 'error')),
    version       integer NOT NULL DEFAULT 0,   -- the upload's version (clans.clan_logo_pending / clan_logo)
    account_id    integer,                      -- who uploaded it: the approval's clan-logo ledger row names them
    sha256        text NOT NULL DEFAULT '',     -- of the normalized WebP; a verdict applies to this upload only
    image         text,                         -- base64 WebP of the approved logo; NULL until one is approved
    bytes         integer NOT NULL DEFAULT 0,   -- the upload's own size
    width         integer NOT NULL DEFAULT 0,
    height        integer NOT NULL DEFAULT 0,
    reason        text NOT NULL DEFAULT '',     -- one neutral sentence for the uploader
    categories    text NOT NULL DEFAULT '',
    model         text NOT NULL DEFAULT '',
    uploaded_at   timestamptz NOT NULL DEFAULT now(),
    moderated_at  timestamptz
);
CREATE INDEX clan_logos_status_idx ON clan_logos (status);
