-- MySQL only: one mm_ranked_lock row per postgres advisory key, id = the kn_rq_lock() offset + 1
-- (postgres takes pg_try_advisory_xact_lock(kn_rq_lock() + k)). Row 1 alone served every lock, so the
-- ranked matcher (db_mm.c rq_lock, row 1), rk_lock 0 / 1 (db_ranked.c, rows 2 / 3) and the turf settlement
-- (db_turf.c, row 17) held each other off: a season close that met the matcher's lock waited 30 s.
INSERT IGNORE INTO mm_ranked_lock (id) VALUES (2), (3), (17);
