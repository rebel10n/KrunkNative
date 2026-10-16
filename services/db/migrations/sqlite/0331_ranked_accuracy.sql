-- 0331_ranked_accuracy (SQLite): same as postgres/0331: the Ranked profile's total_accuracy (v10 average_accuracy =
-- total_accuracy / matches). Additive: old rows read 0.
ALTER TABLE ranked_profiles ADD COLUMN total_accuracy REAL NOT NULL DEFAULT 0;
