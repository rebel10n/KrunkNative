-- 0331_ranked_accuracy (MySQL / MariaDB): same as postgres/0331: the Ranked profile's total_accuracy (v10
-- average_accuracy = total_accuracy / matches; services/brain/db_ranked.c run_rk_match_finish adds each ranked
-- match's accuracy). Additive: old rows read 0. (Missing on MySQL until 2026-11-19: schema_parity_test.py.)
ALTER TABLE ranked_profiles ADD COLUMN total_accuracy DOUBLE NOT NULL DEFAULT 0;
