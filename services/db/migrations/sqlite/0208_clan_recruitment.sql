-- 0208_clan_recruitment (SQLite): same as postgres/0208; Krunker v10.0.2 clan recruitment posts
-- (Turf Wars' Recruitment tab). Owner: services/brain/turf.c + db_turf.c. Additive only.
CREATE TABLE clan_recruitment (
    clan_id       INTEGER PRIMARY KEY REFERENCES clans(clan_id) ON DELETE CASCADE,
    region        TEXT NOT NULL DEFAULT '',
    language      TEXT NOT NULL DEFAULT '',
    description   TEXT NOT NULL DEFAULT '',
    looking_for   TEXT NOT NULL DEFAULT '',
    min_level     INTEGER NOT NULL DEFAULT 0,
    is_active     INTEGER NOT NULL DEFAULT 1,
    min_kd        REAL NOT NULL DEFAULT 0,
    created_by    INTEGER,
    updated_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX clan_recruitment_filter_idx ON clan_recruitment (is_active, region, language, updated_at);

-- the clan bank's movements outside the 0104 deposit / withdraw (v10: a recruitment post costs
-- 1,000 KR from the clan bank)
CREATE TABLE clan_funds_ledger (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    clan_id       INTEGER NOT NULL,
    account_id    INTEGER,                            -- who spent it
    amount        INTEGER NOT NULL,                     -- signed delta
    source        TEXT NOT NULL,                      -- 'recruitment'
    ref           TEXT,
    at            TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
CREATE INDEX clan_funds_ledger_clan_idx ON clan_funds_ledger (clan_id, at);
