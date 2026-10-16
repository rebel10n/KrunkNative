-- 0212_turf_rounds (MySQL/MariaDB): same as postgres/0212; the Turf Wars rounds already counted
-- (match id). Owner: services/brain/db_turf.c. Additive only.
CREATE TABLE turf_rounds (
    match_id     VARCHAR(64) PRIMARY KEY,
    day          INT NOT NULL,
    map_id       INT NOT NULL,
    recorded_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
