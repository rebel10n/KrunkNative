-- 0346_kpd_demos (MySQL/MariaDB): same as postgres/0346; the K.P.D Demos window (v10 kpd.listDemos,
-- tab 4). Nothing writes it yet (no demo recorder); op kpd_demos reads it. Owner: services/brain/db_kpd.c.
-- Additive only. (No functional index: MariaDB has none; the column's default collation matches case.)
CREATE TABLE kpd_demos (
    id             BIGINT AUTO_INCREMENT PRIMARY KEY,
    token          VARCHAR(64) NOT NULL UNIQUE,
    ct             VARCHAR(64) NOT NULL DEFAULT '',
    call_id        BIGINT REFERENCES kpd_calls(id) ON DELETE SET NULL,
    suspect_id     BIGINT REFERENCES accounts(id) ON DELETE SET NULL,
    suspect_name   VARCHAR(32) NOT NULL DEFAULT '',
    suspect_group  INT NOT NULL DEFAULT 0,
    typ            INT NOT NULL DEFAULT 0,
    map            VARCHAR(64) NOT NULL DEFAULT '',
    mode_name      VARCHAR(32) NOT NULL DEFAULT '',
    duration_ms    INT NOT NULL DEFAULT 0,
    banned         SMALLINT NOT NULL DEFAULT 0,
    resolved       SMALLINT NOT NULL DEFAULT 0,
    opened         SMALLINT NOT NULL DEFAULT 1,
    yes_votes      INT NOT NULL DEFAULT 0,
    no_votes       INT NOT NULL DEFAULT 0,
    created_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX kpd_demos_created_idx ON kpd_demos (created_at);
CREATE INDEX kpd_demos_name_idx ON kpd_demos (suspect_name);
