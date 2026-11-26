-- 0410_daily_map_challenges (SQLite): krunker.io's map dailies. A signed-in krunker.io account's Battle Pass
-- Challenges tab, read on 2026-11-25, listed five dailies (their icons are img/chals/icon_<IC>.png):
--   Get a 20 killstreak on Burg    icon 4  +50 BPP  +70 KR  +0 junk     and one Wood (skin 3237, viewItem(3237))
--   Get 3 Headshots on SkyTemple   icon 2  +50 BPP  +0 KR   +0.25 junk
--   Play 1 Burg Match              icon 7  +50 BPP  +0 KR   +0.1 junk
--   Play 1 SkyTemple Match         icon 7  +50 BPP  +0 KR   +0.1 junk
--   Get 15 Headshots               icon 2  +50 BPP  +0 KR   +0.25 junk  (row 15, 0409)
-- v10.0.7's checkChallenge (deob-10.0.7.js:180697) counts "gp" (a match played), the "kstk" streak and a DAT.mp map
-- filter, as shared/src/progression.c does; REW.sk becomes the reward's skins (180865). Burg is mapIndex 0 and
-- SkyTemple 15 (shared/src/map_catalog.c). Row 15 takes krunker.io's name and icon unless an operator changed them.
UPDATE challenges SET icon = 2, name = 'Get 15 Headshots' WHERE challenge_id = 15 AND icon = 1 AND name = '';
INSERT INTO challenges (challenge_id, ctype, dat, rew, icon, name, sort) VALUES
    (27, 'daily', '{"ky":"kstk","val":20,"mp":0}', '{"kr":70,"bpp":50,"sk":[3237]}', 4, 'Get a 20 killstreak on Burg', 48),
    (28, 'daily', '{"ky":"hs","val":3,"mp":15}', '{"kr":0,"bpp":50,"jnk":0.25}', 2, 'Get 3 Headshots on SkyTemple', 49),
    (29, 'daily', '{"ky":"gp","val":1,"mp":0}', '{"kr":0,"bpp":50,"jnk":0.1}', 7, 'Play 1 Burg Match', 54),
    (30, 'daily', '{"ky":"gp","val":1,"mp":15}', '{"kr":0,"bpp":50,"jnk":0.1}', 7, 'Play 1 SkyTemple Match', 55);
