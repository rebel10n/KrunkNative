-- 0351_kpd_demo_votes (SQLite): same as postgres/0351; one K.P.D officer's verdict on a demo (v10 kpd.vote, the
-- demo replay's Yes / No, POST /kpd/demos/{id}/vote, services/brain/v10_mods.c). A verdict replaces the
-- officer's earlier one; kpd_demos.yes_votes / no_votes count them. Additive only.
CREATE TABLE kpd_demo_votes (
    demo_id     INTEGER NOT NULL REFERENCES kpd_demos(id) ON DELETE CASCADE,
    officer_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    verdict     INTEGER NOT NULL DEFAULT 0,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
    PRIMARY KEY (demo_id, officer_id)
);
