-- 0365_skin_stats (MySQL): Box Office counter skins (Krunker v10.0.4: a counter skin's copy counts a stat, its m[stat],
-- deob-10.0.4.js:172726-172735; the server's takeSkinStats adds the match's counts, :191561-191585). Box Office keeps the
-- count per store index: one row per (account, skin, stat), added to by the game server's commit (kn_commit_stats
-- "skinStats") and sent back in the owned list's m (db_owned_skins). Additive only.
CREATE TABLE skin_stats (
    account_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    skin_id     BIGINT NOT NULL CHECK (skin_id >= 0),
    stat        VARCHAR(16) NOT NULL,
    count       BIGINT NOT NULL DEFAULT 0 CHECK (count >= 0),
    PRIMARY KEY (account_id, skin_id, stat)
);
