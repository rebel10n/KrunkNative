-- 0123_account_avatars: profile pictures (services/brain/avatar.c, db_avatar.c). A player uploads
-- a PNG / JPEG / WebP; kn_brain decodes it, crops it to a square, re-encodes it as a 256 px WebP
-- (no upload bytes or metadata are stored) and has it reviewed by the AI moderation model before
-- anyone can see it. One row per account (a new upload replaces the old picture).
--
-- Only an approved picture keeps its image: the image column is NULL while the review runs and
-- after a rejection or a failed review, so content that did not pass is never stored. status:
-- pending (review running), approved (served at /api/avatars/<id>), rejected (reason and
-- categories shown to the owner), error (the review failed: hidden, the owner may upload again).
-- Additive: one new table. Owner: svc-accounts (services/brain).
CREATE TABLE account_avatars (
    account_id    integer PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    status        text NOT NULL CHECK (status IN ('pending', 'approved', 'rejected', 'error')),
    sha256        text NOT NULL DEFAULT '',     -- of the normalized WebP; a verdict applies to this picture only
    image         text,                         -- base64 WebP, approved pictures only
    bytes         integer NOT NULL DEFAULT 0,   -- normalized WebP size
    width         integer NOT NULL DEFAULT 0,   -- the upload's own dimensions
    height        integer NOT NULL DEFAULT 0,
    reason        text NOT NULL DEFAULT '',     -- one neutral sentence for the owner
    categories    text NOT NULL DEFAULT '',     -- comma-separated violated categories
    model         text NOT NULL DEFAULT '',     -- the moderation model that answered
    uploaded_at   timestamptz NOT NULL DEFAULT now(),
    moderated_at  timestamptz
);
CREATE INDEX account_avatars_status_idx ON account_avatars (status);
