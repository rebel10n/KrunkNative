-- 0118_store_v10 (MySQL/MariaDB): same table as postgres/0118_store_v10 (see there).
CREATE TABLE store_spin_data (
    id         INT PRIMARY KEY CHECK (id = 1),
    data       MEDIUMTEXT NOT NULL,
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
