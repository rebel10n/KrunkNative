-- 0403_map_mobile (MySQL / MariaDB): a published map's Mobile Friendly flag, as postgres/0403.
ALTER TABLE published_maps ADD COLUMN map_mobile SMALLINT NOT NULL DEFAULT 0;
