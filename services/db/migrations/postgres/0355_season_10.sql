-- 0355_season_10: Krunker v10.0.3's Season 10 and its Battle Pass (doc/v10/battlepass.md "Season 10").
-- Owner: season-system. Read by kn_brain's db_season.c (P_BP_DATA: the active battlepasses row of
-- accounts.current_season; config.example.json sets 10, shared/include/season.h SEASON_CURRENT 10).
--
-- v10.0.3 plays Season 10 (deob-10.0.3.js L19368 `currentSeason = 10`, L22498). The pass itself
-- reaches the v10 client only over its game socket ("bpdl" DATA_LOADED, L111344): no static file
-- or public API carries it, so this row holds what the bundle and its public data do define and
-- leaves the rest empty (nothing here is invented):
-- - name: the badges' wording "Season 10 Battle Pass" (badge ids 28-30, L116167-116185).
-- - itemcount 80: the official 10.0.3 changelog ("Added new Season 10 Battlepass with 80 levels
--   and 30 new items") and badge 30 ("Complete the Season 10 Battle Pass").
-- - the level costs: the badges' BPP thresholds at level boundaries (L116169-116185): level 60 at
--   25000 BPP (badge 28), level 70 at 39000 (badge 29, VIP), complete at 55000 (badge 30, VIP),
--   i.e. levels 61-70 cost 1400 each and 71-80 1600 each, and levels 1-60 sum to 25000. Their split
--   is not published: 400 a level (bpp_perlevel) and 500 for levels 51-60 (leveldata, v10's
--   "level:bpp" list, L161996-162002) is Krunker Box Office's choice that meets the three sums.
-- - the 30 items: the v10 store flags them (assets/skins.json, gapi /data/skins: seas 10, limT
--   "Battle Pass", battlepass): 7963 Melon Man, 7964 Shuttle Swirl, 7965-7969 Atlantean Armour /
--   Tail / Helm / Cape / Trident, 7970 Pocket Six, 7971 Triple Threat, 7972 Welrod mk2, 7973 R3TR0
--   B4ND, 7974-7980 Vermilion Regalia / Canopy / Sash / Talon / Talisman / Fold / Spirit, 7981-7982
--   GIGA-RED / GIGA-GREEN BIT SKULL, 7983 Atlantis Emblem, 7984 Vermilion Geta, 7985-7989
--   Anti-Eterno Remembers / Jingasa / Vest / Sarong / Ten no Me, 7990 Yamato, 7991 Josuke, 7992
--   Eterno is Greedy. Which level and track (free / VIP) each one sits on, and the KR / junk cards
--   (krdata / jnkdata), come only with "bpdl": itemdata, krdata and jnkdata stay empty until a
--   content decision places them (the cards render empty, as v10 renders a card with no reward).
-- - prices: Krunker Box Office sells the VIP tier and level skips for KR (0108_seasons: 1200 KR,
--   200 KR a level; v10 charges USD through Xsolla, tier1price from "bpdl", levelSkipPrice "0.99"
--   L19421). The dates are not published either: the pass starts when this migration runs and
--   lasts 180 days, like the Season 7 row.
INSERT INTO seasons (season, name) VALUES (10, 'Season 10') ON CONFLICT (season) DO NOTHING;

INSERT INTO battlepasses (bp_id, season, name, descr, bpp_perlevel, itemcount, tier1_name, tier1_price_kr, skip_price_kr, itemdata, leveldata, krdata, jnkdata, startdate, enddate)
    VALUES (3, 10, 'Season 10 Battle Pass', 'Krunker Season 10', 400, 80, 'VIP', 1200, 200, '',
    '51:500,52:500,53:500,54:500,55:500,56:500,57:500,58:500,59:500,60:500,61:1400,62:1400,63:1400,64:1400,65:1400,66:1400,67:1400,68:1400,69:1400,70:1400,71:1600,72:1600,73:1600,74:1600,75:1600,76:1600,77:1600,78:1600,79:1600,80:1600',
    '', '', now(), now() + interval '180 days') ON CONFLICT (bp_id) DO NOTHING;
