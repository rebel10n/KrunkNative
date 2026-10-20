-- 0350_guest_profiles (PostgreSQL): the signed-out guest's balance. Krunker v10.0.2 keeps a guest's earned
-- KR and score on the server (GET /guest/profile, the credit at round end, the claim at login); the routes
-- are services/brain/guest.c, the procs db_guest.c, the game servers' ops guest_credit / guest_claim in
-- accounts.c. Additive only.
-- guest_id is the client-generated id: 32 lowercase hex digits, a bearer identifier (checked in C, never
-- logged whole). A match credits once per (guest, match): its guest_credits row is inserted first, and only
-- a new row moves the balance. The caps (KR 20, score 2500: v10's guestKRCap / guestScoreCap) are applied by
-- the proc, not stored here. A claim zeroes funds and score and keeps the credit rows, so a replayed match
-- never pays again.
CREATE TABLE guest_profiles (
    guest_id    TEXT PRIMARY KEY,
    funds       BIGINT NOT NULL DEFAULT 0 CHECK (funds >= 0),
    score       BIGINT NOT NULL DEFAULT 0 CHECK (score >= 0),
    updated_at  timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE guest_credits (
    guest_id    TEXT NOT NULL,
    match_key   TEXT NOT NULL CHECK (match_key <> ''),
    kr          BIGINT NOT NULL CHECK (kr >= 0),
    xp          BIGINT NOT NULL CHECK (xp >= 0),
    at          timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (guest_id, match_key),
    FOREIGN KEY (guest_id) REFERENCES guest_profiles(guest_id) ON DELETE CASCADE
);
