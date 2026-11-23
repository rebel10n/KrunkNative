-- 0394_custom_skins (MySQL / MariaDB): same as postgres/0394. Krunker v10.0.7's Skin Manager records behind kn_brain
-- (services/brain/skins.c, P_CUSTOM_SKIN). Additive only.
CREATE TABLE custom_skins (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    owner_id    BIGINT NOT NULL,
    name        VARCHAR(255) NOT NULL,
    type        SMALLINT NOT NULL DEFAULT 0,
    weapon      SMALLINT NOT NULL DEFAULT 0,
    rarity      SMALLINT NOT NULL DEFAULT 0,
    item_config MEDIUMTEXT NOT NULL,
    created_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at  DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (owner_id) REFERENCES accounts(id) ON DELETE CASCADE
);
CREATE INDEX custom_skins_owner_idx ON custom_skins (owner_id, id);
