-- 0395_skin_assets (MySQL / MariaDB): same as postgres/0395_skin_assets (see there).
-- AUTO_INCREMENT never goes below the table's max(id) + 1
ALTER TABLE custom_skins AUTO_INCREMENT = 1000000000;
CREATE TABLE studio_skins (
    id             BIGINT AUTO_INCREMENT PRIMARY KEY,
    studio_id      BIGINT NOT NULL,
    custom_skin_id BIGINT NOT NULL,
    name           VARCHAR(255) NOT NULL,
    type           SMALLINT NOT NULL DEFAULT 0,
    weapon         SMALLINT NOT NULL DEFAULT 0,
    rarity         SMALLINT NOT NULL DEFAULT 0,
    item_config    MEDIUMTEXT NOT NULL,
    active         SMALLINT NOT NULL DEFAULT 0,
    created_by     BIGINT NULL,
    created_at     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (studio_id) REFERENCES studios(id) ON DELETE CASCADE,
    FOREIGN KEY (created_by) REFERENCES accounts(id) ON DELETE SET NULL
);
CREATE UNIQUE INDEX studio_skins_studio_skin_uq ON studio_skins (studio_id, custom_skin_id);
ALTER TABLE studio_skins AUTO_INCREMENT = 10000001;
