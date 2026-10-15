-- 0209_kpd_calls: the Krunker v10.0.2 K.P.D (Krunker Police Department) distress calls: a player's
-- "Call K.P.D" from the in-game player list (v10 "kpd" packet), recorded by
-- the game server through op kpd_call (services/brain/db_kpd.c). The officers' Distress Calls tab
-- lists the open ones (kpd_calls), the moderation log the handled ones (kpd_log). An officer who joins
-- the suspect's lobby rates the call (v10 "rkpd", op kpd_rate): rating 1 tagged (hacking), 0 not
-- tagged; status goes open -> handled. The reporter's helpful-report rating (v10 jobRatingPos /
-- jobRating) is derived from their rated calls. Owner: services/brain/db_kpd.c. Additive only.
CREATE TABLE kpd_calls (
    id             bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    reporter_id    integer REFERENCES accounts(id) ON DELETE SET NULL,
    reporter_name  text NOT NULL DEFAULT '',
    reporter_level integer NOT NULL DEFAULT 0,
    suspect_id     integer REFERENCES accounts(id) ON DELETE SET NULL,  -- NULL: a guest
    suspect_name   text NOT NULL DEFAULT '',
    lobby          text NOT NULL DEFAULT '',         -- "<node>:<lobby id>" (the Join button's game)
    node           text NOT NULL DEFAULT '',
    region         text NOT NULL DEFAULT '',
    map            text NOT NULL DEFAULT '',
    mode           integer NOT NULL DEFAULT 0,       -- Krunker mode id
    typ            integer NOT NULL DEFAULT 0,       -- v10 call type: 0 public, 1 ranked, 2 comp, 3 custom
    status         integer NOT NULL DEFAULT 0,       -- 0 open, 1 handled (rated by an officer), 2 expired
    rating         smallint,                         -- the officer's verdict: NULL unrated, 1 tagged (hacking), 0 not tagged
    handled_by     integer,                          -- the officer's account
    handled_at     timestamptz,
    created_at     timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX kpd_calls_open_idx ON kpd_calls (status, created_at DESC);
CREATE INDEX kpd_calls_suspect_idx ON kpd_calls (suspect_id, created_at DESC);
CREATE INDEX kpd_calls_reporter_idx ON kpd_calls (reporter_id, status);
