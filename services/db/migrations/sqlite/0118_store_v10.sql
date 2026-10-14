-- 0118_store_v10 (SQLite): same table as postgres/0118_store_v10 (see there).
CREATE TABLE store_spin_data (
    id         INTEGER PRIMARY KEY CHECK (id = 1),
    data       TEXT NOT NULL,
    updated_at TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
);
