-- 0209_kpd_calls (MySQL/MariaDB): same as postgres/0209; the Krunker v10.0.2 K.P.D distress calls
-- from the in-game player list ("Call K.P.D"). Owner: services/brain/db_kpd.c. Additive only.
CREATE TABLE kpd_calls (
    id             BIGINT AUTO_INCREMENT PRIMARY KEY,
    reporter_id    BIGINT,
    reporter_name  VARCHAR(32) NOT NULL DEFAULT '',
    reporter_level INT NOT NULL DEFAULT 0,
    suspect_id     BIGINT,
    suspect_name   VARCHAR(32) NOT NULL DEFAULT '',
    lobby          VARCHAR(32) NOT NULL DEFAULT '',
    node           VARCHAR(16) NOT NULL DEFAULT '',
    region         VARCHAR(32) NOT NULL DEFAULT '',
    map            VARCHAR(64) NOT NULL DEFAULT '',
    mode           INT NOT NULL DEFAULT 0,
    typ            INT NOT NULL DEFAULT 0,
    status         INT NOT NULL DEFAULT 0,
    rating         SMALLINT,
    handled_by     BIGINT,
    handled_at     DATETIME(3),
    created_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX kpd_calls_open_idx ON kpd_calls (status, created_at);
CREATE INDEX kpd_calls_suspect_idx ON kpd_calls (suspect_id, created_at);
CREATE INDEX kpd_calls_reporter_idx ON kpd_calls (reporter_id, status);
