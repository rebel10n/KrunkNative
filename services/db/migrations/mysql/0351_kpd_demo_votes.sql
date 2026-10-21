-- 0351_kpd_demo_votes (MySQL/MariaDB): same as postgres/0351; one K.P.D officer's verdict on a demo (v10
-- kpd.vote, POST /kpd/demos/{id}/vote, services/brain/v10_mods.c). A verdict replaces the officer's earlier
-- one; kpd_demos.yes_votes / no_votes count them. Additive only.
CREATE TABLE kpd_demo_votes (
    demo_id     BIGINT NOT NULL REFERENCES kpd_demos(id) ON DELETE CASCADE,
    officer_id  BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    verdict     INT NOT NULL DEFAULT 0,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (demo_id, officer_id)
);
