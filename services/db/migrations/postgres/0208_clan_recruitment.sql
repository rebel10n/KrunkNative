-- 0208_clan_recruitment: Krunker v10.0.2 clan recruitment posts (v10's gapi clans client:
-- /clans/recruitment, /clans/<id>/recruitment, /clans/<id>/apply, /clans/my-applications), browsed
-- in the Turf Wars popup's Recruitment tab and written from the clan settings. Owner:
-- services/brain/turf.c + db_turf.c. Additive only: one new table; an application is a row of the
-- existing 0101 clan_requests (the clan's Requests list).
CREATE TABLE clan_recruitment (
    clan_id       integer PRIMARY KEY REFERENCES clans(clan_id) ON DELETE CASCADE,  -- one post per clan
    region        text NOT NULL DEFAULT '',        -- '' / NA EU AS OCE SA ME
    language      text NOT NULL DEFAULT '',        -- '' / en es pt fr de ...
    description   text NOT NULL DEFAULT '',        -- <= 2000 characters (v10's textarea)
    looking_for   text NOT NULL DEFAULT '',        -- <= 200 characters
    min_level     integer NOT NULL DEFAULT 0,
    is_active     boolean NOT NULL DEFAULT true,       -- a removed post stays (re-posting costs again)
    min_kd        double precision NOT NULL DEFAULT 0,
    created_by    integer,
    updated_at    timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX clan_recruitment_filter_idx ON clan_recruitment (is_active, region, language, updated_at DESC);

-- the clan bank's movements outside the 0104 deposit / withdraw (v10: a recruitment post costs
-- 1,000 KR from the clan bank)
CREATE TABLE clan_funds_ledger (
    id            bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    clan_id       integer NOT NULL,
    account_id    integer,                            -- who spent it
    amount        bigint NOT NULL,                     -- signed delta
    source        text NOT NULL,                      -- 'recruitment'
    ref           text,
    at            timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX clan_funds_ledger_clan_idx ON clan_funds_ledger (clan_id, at);
