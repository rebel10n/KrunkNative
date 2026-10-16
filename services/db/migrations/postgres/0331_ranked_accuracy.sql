-- 0331_ranked_accuracy: the Ranked profile's total_accuracy (v10 profile: average_accuracy = total_accuracy /
-- matches, v10:34033 / 98337). The brain adds each ranked match's accuracy (hits / shots x 100, the ranked
-- result's shots and hits, services/brain/db_ranked.c run_rk_match_finish). Additive: old rows read 0.
ALTER TABLE ranked_profiles ADD COLUMN total_accuracy double precision NOT NULL DEFAULT 0;
