-- 0397_daily_challenge_bpp (PostgreSQL): the daily challenges' Battle Pass points. Krunker v10.0.7's daily cards
-- (genDailyCard, deob-10.0.7.js:219232-219243) and challenge cards (genChallengeCard 221427-221458) draw REW.bpp, and
-- finalize credits it to the season (180855-180902); 0107 seeded the dailies (15-22) with KR only. No capture of the
-- official dailies' bpp is in the repo: the values follow the offline v10 sandbox's daily rows
-- (tools/uidiff/scenarios/rewards-bp-chal.json: 50, the sprays' 25). A row an operator already changed is left alone.
UPDATE challenges SET rew = '{"kr":5,"bpp":50}' WHERE challenge_id = 15 AND rew = '{"kr":5}';
UPDATE challenges SET rew = '{"kr":5,"bpp":50}' WHERE challenge_id = 16 AND rew = '{"kr":5}';
UPDATE challenges SET rew = '{"kr":10,"bpp":50}' WHERE challenge_id = 17 AND rew = '{"kr":10}';
UPDATE challenges SET rew = '{"kr":10,"bpp":50}' WHERE challenge_id = 18 AND rew = '{"kr":10}';
UPDATE challenges SET rew = '{"kr":5,"bpp":50}' WHERE challenge_id = 19 AND rew = '{"kr":5}';
UPDATE challenges SET rew = '{"kr":5,"bpp":25}' WHERE challenge_id = 20 AND rew = '{"kr":5}';
UPDATE challenges SET rew = '{"kr":5,"bpp":25}' WHERE challenge_id = 21 AND rew = '{"kr":5}';
UPDATE challenges SET rew = '{"kr":10,"bpp":50}' WHERE challenge_id = 22 AND rew = '{"kr":10}';
