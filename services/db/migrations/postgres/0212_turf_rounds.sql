-- 0212_turf_rounds: the Turf Wars rounds already counted (services/brain/db_turf.c). A game server
-- resends its reliable "turf-kills" frame when the reply was lost on a link drop; the round's
-- match id is kept here so the same round never counts twice. Owner: svc-social (turf.c).
-- Additive only: one new table.
CREATE TABLE turf_rounds (
    match_id     text PRIMARY KEY,
    day          integer NOT NULL,
    map_id       integer NOT NULL,
    recorded_at  timestamptz NOT NULL DEFAULT now()
);
