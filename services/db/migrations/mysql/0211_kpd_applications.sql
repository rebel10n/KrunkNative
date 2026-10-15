-- 0211_kpd_applications (MySQL/MariaDB): same as postgres/0211; K.P.D applications ("kpdA").
-- Owner: services/brain/db_kpd.c. Additive only.
CREATE TABLE kpd_applications (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account_id  BIGINT NOT NULL,
    name        VARCHAR(32) NOT NULL DEFAULT '',
    discord     VARCHAR(40) NOT NULL DEFAULT '',
    reason      VARCHAR(300) NOT NULL DEFAULT '',
    status      INT NOT NULL DEFAULT 0,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    closed_at   DATETIME(3)
);
CREATE INDEX kpd_applications_status_idx ON kpd_applications (status, created_at);
CREATE INDEX kpd_applications_account_idx ON kpd_applications (account_id, status);
