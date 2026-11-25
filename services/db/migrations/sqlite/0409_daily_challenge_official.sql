-- 0409_daily_challenge_official (SQLite): the daily rewards as krunker.io sends them. Read on 2026-11-25 from a
-- signed-in krunker.io account's Battle Pass Challenges tab (windows[5].switchTab(1), genDailyCard
-- deob-10.0.7.js:219232-219243), five dailies:
--   Get 15 Headshots                   +50 BPP  +0 KR   +0.25 junk
--   Get 3 Headshots on SkyTemple       +50 BPP  +0 KR   +0.25 junk
--   Play 1 Burg Match                  +50 BPP  +0 KR   +0.1 junk
--   Play 1 SkyTemple Match             +50 BPP  +0 KR   +0.1 junk
--   Get a 20 killstreak on Burg        +50 BPP  +70 KR  +0 junk
-- Every official daily gives 50 BPP, so the spray and assist rows (20, 21) move from 0397's 25 to 50. Row 15 is
-- "Get 15 Headshots" itself, and row 18 (5 headshots with one weapon) takes the same headshot reward. The other rows
-- have no official counterpart in the read and keep their KR. A row an operator already changed is left alone.
UPDATE challenges SET rew = '{"kr":0,"bpp":50,"jnk":0.25}' WHERE challenge_id = 15 AND rew = '{"kr":5,"bpp":50}';
UPDATE challenges SET rew = '{"kr":0,"bpp":50,"jnk":0.25}' WHERE challenge_id = 18 AND rew = '{"kr":10,"bpp":50}';
UPDATE challenges SET rew = '{"kr":5,"bpp":50}' WHERE challenge_id = 20 AND rew = '{"kr":5,"bpp":25}';
UPDATE challenges SET rew = '{"kr":5,"bpp":50}' WHERE challenge_id = 21 AND rew = '{"kr":5,"bpp":25}';
