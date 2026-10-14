# Krunker v10.0.2 client bundle: overview

Source: `~/Downloads/v10.0.2.js` (13 586 176 bytes, 223 181 lines, sha not recorded). It is one
IIFE (`!function(){'use strict'; ... }(); //# sourceMappingURL=app.js.map`). Identifiers are
obfuscated (`iîïíîìi`, `IÌíïííí`); strings, property names and numbers are mostly intact (hex
literals). Never executed. Line numbers below are in the renamed copy
`~/.cache/kn-v10/v10.js` (made by `~/.cache/kn-v10-tools/rename.py`: identifiers -> `_N`, hex -> decimal,
`\x20`-style escapes decoded). The line count is the same as the original, so the line numbers also hold
for the original file.

The v7.1.4 reference is `/home/user/jails/kru/src/Krunker_v7.1.4` (readable `src/`, webpack modules
split into files). "v7 file:line" below refers to that tree.

## Version strings

| Item | v10 value | v10.js | v7.1.4 | v7 file:line |
|---|---|---|---|---|
| gameVersion | `'10.0.1'` (the bundle says 10.0.1, not 10.0.2) | 16404 `var _7259 = '10.0.1'`, 19686 | `"7.1.4"` | src/config.js:15 |
| clientVersion / clientRevision | `'10.0.1' + 'r1'`, `"1"` (new) | 16405 | none | - |
| editorVersion | `"2.0.0"` (unchanged) | 19634 | `"2.0.0"` | src/config.js:16 |
| buildVersion | `"FArgZqd2pNs7O8lWk4TblHSdDWfMUfD2"` | config object | `"egh9QTeI..."` | src/config.js:17 |
| currentSeason | 10 | 16499 `_7305` | 7 | src/config.js |
| THREE.REVISION | `'186'` | 28247 | `"150"` (package.json `three ^0.150.1`) | public/js/viewer.js:9973 |
| core-js | 3.39.0 | 810 | - | - |
| map fetch cache key | `maps/<file>.json?v=DUA9R` (FTUE bot maps only) | 222925 | - | - |
| asset build query | `?build=4Q9IjxljZogSd4f5mvT6QLSHsyBGeq8I` | 16593 | - | - |

## Module map (line ranges in v10.js)

The ranges are approximate, found from string density. "Client" means browser-only code; "shared" means
code that also branches on `isServer` (the same bundle carries the game simulation the server runs).

| Lines | Module | Notes |
|---|---|---|
| 1-15 000 | Babel helpers, core-js 3.39 polyfills, URL polyfill, QR-code encoder (12 000-14 200) | library |
| 11 856-12 000 and spread through the file | Svelte 5 runtime + components (`svelte-<hash>` scoped CSS, 212 component hashes, `__click` delegated events) | new: v7 had no Svelte. Much of the v10 UI (main menu mount `mainMenuUIMount`, notification centre, store, KID/GDC parental-consent flow) is Svelte |
| 15 000-16 400 | Movement presets, account/menu helpers | |
| 16 404-20 114 | `config` (frozen object, 19 477-20 114, ~640 keys) and its constants | the v10 equivalent of src/config.js. Diff: `~/.cache/kn-v10/cfgdiff.txt`, summarised below |
| 20 000-27 600 | msgpack-style encoder/decoder (`ensureBufferSizeToWrite`, `decodeExtension`), WebSocket helpers (`createWebSocket` 24 000ff), REST API client `new _10077(gapiURL)` (26 000-27 600: shop, lottery, maps, leaderboards, clans, ranked) | the REST client is new: v7 used the socket for most of these |
| 27 600-92 000 | THREE r186: core, WebGLRenderer, **WebGPURenderer + TSL node system** (51 000-90 771), loaders | v7 used THREE r150 with WebGLRenderer only |
| 92 000-98 000 | Game shaders (paint shader, TSL node materials), captcha (Cloudflare Turnstile), matchmaking state (`currentMatchmakingState` 96 000) | |
| 98 000-98 500 | Game socket client (`socketReady`, `new WebSocket` 98 111/98 439) | net.md |
| 98 700-101 000 | Weapons table (`"Sniper Rifle"` 98 983) | gameplay.md |
| 101 900-102 100 | Classes (`"Triggerman"` 101 962, `"Hunter"` 101 981) | gameplay.md |
| 102 700-103 000 | Store data: item pools, wheels, rarity table, skin categories (`'Clan Wars'` 102 781 colour tag) | |
| 104 000-106 000 | Game modes (`"Hardpoint"` 104 081) | gameplay.md |
| 105 000-147 000 | Menus: end screen, store, inventory, market, studios, leaderboards, clans and Territory Wars (141 000-142 300), server browser (`"Free for All"` 144 164), web push | ui.md, clans.md |
| 147 000-154 000 | Matchmaker client (147 380-147 600), GLTF/FBX loaders | ranked.md |
| 154 000-167 000 | Map manager: prefabs (`SPAWN_POINT` 154 525), textures, triggers and KrunkScript (`trigConstTx` 162 637), map lights (`addPointLight`/`addSpotLight`/`addRectAreaLight` 165 788) | map-format.md |
| 167 000-191 500 | Game simulation (shared server/client: `isServer` 166 954-191 513): maps, players, movement (`this["jump"]` 172 009), weapons, projectiles, bombs, streaks, `clanWar` 170 666 | gameplay.md |
| 192 000-202 000 | Renderer setup (`forceWebGL`, `kro_setngss_webgpu` 192 083), LTC tables for RectAreaLight (192 049), scene/sky/shadows (`skyLight` 199 086), post-processing (199 109-204 014) | |
| 197 599-197 612 | i18n: the English string table inline (`translations['en'] = parseText("...")`, 2 172 keys; dump: `~/.cache/kn-v10/i18n_en.txt`) | v7 loaded i18n separately (src/i18n) |
| 202 000-223 181 | App bootstrap: windows (legacy `windows[N]` innerHTML UI), HUD, settings, crosshair, social, map list (`maps.getMaps()` 222 889), FTUE bot match (`bot_ftue_v2`) | ui.md |

## Client vs embedded data

| Data | Where it lives in v10 | v7.1.4 |
|---|---|---|
| Weapons, classes, modes, prefabs, triggers, movement, config | embedded in the bundle (as in v7) | src/data/*.js, src/config.js |
| Official map list | **fetched**: `GET {gapiURL}/maps` (`maps.getMaps`, 26 577, used at 222 889), single map `GET /maps/<id>` with `cache:'no-cache'` (219 048) | embedded src/data/maps.js (32 entries) |
| Map JSON | fetched per map from gapi (see above). Only the FTUE bot maps come from `maps/<file>.json` | maps/<file>.json on the game host |
| i18n (English) | embedded (197 610) | separate files under src/i18n |
| Ranked map pool | computed from the date (`getRankedMaps`, 16 518) | `tmpRankTms` / comp map list in config |
| Store/rarity/skin tables | embedded (102 700ff) | src/data/store.js, rarities.js |

## Services (config getters, 16 439)

| Getter | Prod value | Notes |
|---|---|---|
| matchmakerURL | `https://matchmaker.krunker.io` | env MATCHMAKER_URI |
| apiURL | `https://api.krunker.io` | env API_URI; also `ipCheckerURL = apiURL + "/ipinfo"` |
| gapiURL | `https://gapi.svc.krunker.io` | env GAPI_URI; base of the REST client (`new _10077(_7284)` 27 600) |
| socialURL | `https://social.krunker.io` | |
| editorURL | `https://editor.krunker.io` | |
| gamefrontendURL | `https://gamefrontend.svc.krunker.io` | new |
| communityURL | `https://community.krunker.io` | new |
| eventBusURL | (getter, 19 660) | new |
| beta variants | `*.beta.krunker.io` | `betaSchemes`, `betaURLs` new |
| prodURLs | `["krunker.io", "browserfps.com"]` | v7: krunker.io, krunker.frvr.com, a run.app host |
| regions | `de-fra, us-ca-sv, au-syd, jb-hnd, sgp, us-nj, as-mb, us-tx, brz, me-bhn` (+`sss` off-prod); dedicated-only: `us-wa, us-il, gb-lon, mx-cmx, as-seoul`; `regionIndexNames {1:oc,2:as,3:eu,4:na,5:sa}` | 16 442-16 478 |

## Config diff summary (v10 config object vs v7.1.4 src/config.js)

Full list: `~/.cache/kn-v10/cfgdiff.txt` (made by `~/.cache/kn-v10-tools/cfgdiff.py`). Keys a port has to
care about:

| Change | Keys | Doc |
|---|---|---|
| Ranked (new) | `RANKED_SEASON_START_YEAR 2025`, `_MONTH 8`, `RANKED_ALWAYS_MAP 2`, `RANKED_ROTATION_SLOTS 4`, `currentRankedSeason`, `getRankedMaps`, `rankedMapPoolInfo`, `rankedTimeForFirstConnect 60000`, `voteForfeitTimer 60000`, `voteForfeitDisconnectGrace 30000`, `gameModesSavingHistory [2]` | ranked.md |
| Ranked (removed from the client) | `eloK 32`, `eloPer 120`, `minRankedLevel 15`, `rankedCooldown 7200000`, `rankedDeduction 30`, `tmpRankTms` | ranked.md |
| Clans | `TERRITORY_WARS_INTRO_DATE "2026-03-17"`, `territorialWarsRewardSplit [0.6,0.3,0.1]` | clans.md |
| Raids | `RAID_MONTHLY_START_YEAR 2026`, `_MONTH 3`, `currentRaidMonth` | gameplay.md |
| Map/logic | `archetypeLimit 32`, `archetypeObjectLimit 40`, `entityLimit 64`, `entityStateBytes 512`, `entityStateKeys 32`, `entityStateKeyLen 20`, `entityCtrlNetRate 60`, `entityCorrectionsPerTick 2`, `entityReplayCap 64`, `entitySimBudgetMs 2`, `triggerActionLimit 5 -> 8`, `lightIntensityScaling Math.PI` | map-format.md, net.md |
| Movement keys dropped from config | `playerSpeed`, `airSpeed`, `EPM`, `cornerGlide`, `landPull*`, `terrain*` are no longer config entries. v7 game code never read them (dead constants); the movement values themselves are unchanged and inlined (e.g. jump 0.072 at 172 018) | gameplay.md |
| Account/economy | `usernamePriceUSD 9.99`, `usernameValidDays 365`, `usernameSwapDays 7`, `guestKRCap 20`, `guestScoreCap 2500`, `ftueKRGift 150`, `rewardTime 43200000 -> 3600000`, `bppCapDay 300 -> 500`, level gates `giftMinLVl 30->10`, `marketMinLVl 15->10`, `tradeMinLvl 20->10` | ui.md |
| Captcha | `turnstileSiteKey` (Cloudflare Turnstile) replaces `hcaptchaKey` / `recaptchaKey` | net.md |
| Payments | `stripeEnabled`, `xsollaLocalTesting` | ui.md |
| Hosting | `hostTransferLimit 2 -> 5`, `shutdownTimer 30000`, `dediMaxPlr` removed | |
| Removed features | billboards (`billb*`), bids, partner/business tabs, voice chat timers (`voiceChatInterval`, `voiceDelay`), `twitchLinking`, `themes`, `newRecoil`, `hpSegments` | ui.md |

## Renderer (port impact)

v10 moved to THREE r186. The renderer factory (192 082-192 120) builds a `WebGPURenderer` that falls back
to WebGL2 (`forceWebGL` unless `navigator.gpu` and the setting `kro_setngss_webgpu` is "true"). Materials
are TSL node materials. `lightIntensityScaling = Math.PI` reflects r155+ physically-correct light units:
map light intensities are multiplied by PI to look like v7 (guess, see map-format.md). Shadow map type
setting: 0/1 (PCFSoft was removed in r186, 77 624). Map lights now include `addRectAreaLight` (only when
the lighting setting is 3, 200 307) on top of point and spot lights. KrunkNative's GL renderer does not need
WebGPU. It needs the light-unit change (PI factor) and, optionally, rect-area lights.

## Related documents

map-format.md, gameplay.md, net.md, ranked.md, clans.md, ui.md (this directory). Krunker v10.0.2 is the
only version (doc/v7/decisions.md); the version toggle (v3.1.1 / v7.1.4 / v10.0.2) was removed.
