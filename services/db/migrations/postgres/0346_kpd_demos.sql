-- 0346_kpd_demos: the K.P.D Demos window (v10 kpd.listDemos, tab 4). One row per recorded demo of a
-- suspect: the demo's token (tok) and its call token (ct) as v10 links them, the call it came from
-- (kpd_calls, NULL when none), the suspect (suspect_id: an account, NULL for a guest, who is then
-- grouped by suspect_group), the match (typ: 0 public, 1 ranked, 2 comp, 3 custom), and the case's
-- status: banned, resolved, opened (open for officers' votes), yes / no votes. There is no demo
-- recorder yet, so nothing writes this table: op kpd_demos (services/brain/db_kpd.c) only reads it.
-- Owner: services/brain/db_kpd.c. Additive only.
CREATE TABLE kpd_demos (
    id             bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    token          text NOT NULL UNIQUE,
    ct             text NOT NULL DEFAULT '',
    call_id        bigint REFERENCES kpd_calls(id) ON DELETE SET NULL,
    suspect_id     integer REFERENCES accounts(id) ON DELETE SET NULL,  -- NULL: a guest
    suspect_name   text NOT NULL DEFAULT '',
    suspect_group  integer NOT NULL DEFAULT 0,       -- the suspect's group number (a guest's identity)
    typ            integer NOT NULL DEFAULT 0,       -- v10 call type: 0 public, 1 ranked, 2 comp, 3 custom
    map            text NOT NULL DEFAULT '',
    mode_name      text NOT NULL DEFAULT '',
    duration_ms    integer NOT NULL DEFAULT 0,       -- the demo's length in milliseconds (v10 "dur")
    banned         smallint NOT NULL DEFAULT 0,      -- the suspect was banned on this case
    resolved       smallint NOT NULL DEFAULT 0,      -- the case is closed
    opened         smallint NOT NULL DEFAULT 1,      -- the case is open for the officers' votes
    yes_votes      integer NOT NULL DEFAULT 0,
    no_votes       integer NOT NULL DEFAULT 0,
    created_at     timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX kpd_demos_created_idx ON kpd_demos (created_at DESC);
CREATE INDEX kpd_demos_name_idx ON kpd_demos (lower(suspect_name));
