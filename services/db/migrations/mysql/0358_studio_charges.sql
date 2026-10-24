-- 0358_studio_charges (MySQL/MariaDB): same as postgres/0358: the KR monetized studios' games charge their players
-- after the consent ("chreq" / "chrsp", db_studios.c P_STUDIO_CHARGE): the gross KR paid and the studio's 90 % share,
-- one row per charge, req_key unique (a resent charge never takes the KR twice). Additive.
CREATE TABLE studio_charges (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    req_key     VARCHAR(96) NOT NULL UNIQUE,
    studio_id   BIGINT NOT NULL,
    map_id      BIGINT NOT NULL,
    account_id  BIGINT NOT NULL,
    amount      BIGINT NOT NULL CHECK (amount > 0),
    earned      BIGINT NOT NULL CHECK (earned >= 0),
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX studio_charges_studio_idx ON studio_charges (studio_id, created_at);
CREATE INDEX studio_charges_account_idx ON studio_charges (account_id, created_at);
