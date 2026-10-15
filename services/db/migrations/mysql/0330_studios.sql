-- 0330_studios (MySQL/MariaDB): same tables as postgres/0330; Krunker v10.0.2's Studios (services/brain/db_studios.c).
-- The case-insensitive collation makes UNIQUE (name) lower(name)-unique, as accounts.username.
CREATE TABLE studios (
    id                   BIGINT AUTO_INCREMENT PRIMARY KEY,
    name                 VARCHAR(32) NOT NULL,
    owner_id             BIGINT NOT NULL,
    monetization         SMALLINT NOT NULL DEFAULT 0,
    monetization_blocked SMALLINT NOT NULL DEFAULT 0,
    created_at           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE UNIQUE INDEX studios_name_lower_uq ON studios (name);
CREATE INDEX studios_owner_idx ON studios (owner_id);

CREATE TABLE studio_members (
    studio_id  BIGINT NOT NULL,
    account_id BIGINT NOT NULL,
    role       SMALLINT NOT NULL DEFAULT 2,
    joined_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (studio_id, account_id)
);
CREATE INDEX studio_members_account_idx ON studio_members (account_id);

CREATE TABLE studio_invites (
    id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    studio_id  BIGINT NOT NULL,
    account_id BIGINT NOT NULL,
    invited_by BIGINT,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    expires_at DATETIME(3) NOT NULL
);
CREATE UNIQUE INDEX studio_invites_uq ON studio_invites (studio_id, account_id);
CREATE INDEX studio_invites_account_idx ON studio_invites (account_id, expires_at);

CREATE TABLE studio_maps (
    map_id     BIGINT PRIMARY KEY,
    studio_id  BIGINT NOT NULL,
    added_by   BIGINT,
    added_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX studio_maps_studio_idx ON studio_maps (studio_id);

CREATE TABLE studio_currencies (
    id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    studio_id  BIGINT NOT NULL,
    name       VARCHAR(32) NOT NULL,
    symbol     VARCHAR(8) NOT NULL,
    decimals   SMALLINT NOT NULL DEFAULT 0,
    active     SMALLINT NOT NULL DEFAULT 1,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE UNIQUE INDEX studio_currencies_symbol_uq ON studio_currencies (studio_id, symbol);

CREATE TABLE studio_monetization (
    id           BIGINT AUTO_INCREMENT PRIMARY KEY,
    studio_id    BIGINT NOT NULL,
    applicant_id BIGINT,
    description  TEXT NOT NULL,
    map_ids      VARCHAR(255) NOT NULL DEFAULT '[]',
    status       VARCHAR(16) NOT NULL DEFAULT 'pending',
    review_note  TEXT,
    reviewed_by  BIGINT,
    reviewed_at  DATETIME(3),
    created_at   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
CREATE INDEX studio_monetization_studio_idx ON studio_monetization (studio_id, created_at);
