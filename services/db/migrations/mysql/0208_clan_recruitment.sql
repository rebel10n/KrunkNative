-- 0208_clan_recruitment (MySQL/MariaDB): same as postgres/0208; Krunker v10.0.2 clan recruitment
-- posts (Turf Wars' Recruitment tab). Owner: services/brain/turf.c + db_turf.c. Additive only.
CREATE TABLE clan_recruitment (
    clan_id       BIGINT PRIMARY KEY,
    region        VARCHAR(8) NOT NULL DEFAULT '',
    language      VARCHAR(8) NOT NULL DEFAULT '',
    description   VARCHAR(2000) NOT NULL DEFAULT '',
    looking_for   VARCHAR(200) NOT NULL DEFAULT '',
    min_level     INT NOT NULL DEFAULT 0,
    is_active     TINYINT(1) NOT NULL DEFAULT 1,
    min_kd        DOUBLE NOT NULL DEFAULT 0,
    created_by    BIGINT,
    updated_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (clan_id) REFERENCES clans(clan_id) ON DELETE CASCADE
);
CREATE INDEX clan_recruitment_filter_idx ON clan_recruitment (is_active, region, language, updated_at);

-- the clan bank's movements outside the 0104 deposit / withdraw (v10: a recruitment post costs
-- 1,000 KR from the clan bank)
CREATE TABLE clan_funds_ledger (
    id            BIGINT AUTO_INCREMENT PRIMARY KEY,
    clan_id       BIGINT NOT NULL,
    account_id    BIGINT,                            -- who spent it
    amount        bigint NOT NULL,                     -- signed delta
    source        VARCHAR(32) NOT NULL,                      -- 'recruitment'
    ref           VARCHAR(64),
    at            DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX clan_funds_ledger_clan_idx ON clan_funds_ledger (clan_id, at);
