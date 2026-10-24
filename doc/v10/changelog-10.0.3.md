# Krunker v10.0.3: changelog and KrunkNative parity

Date of this review: 2026-10-21. Base: Krunker v10.0.2 (`doc/v10/`, the reference KN's values were ported from; the target is v10.0.3, doc/v10/target.md).

## Sources

- **Official changelog (authoritative).** Krunker's own `docs/versions.txt`, fetched through the
  local proxy on 2026-10-21 (saved outside the repo as `~/.cache/kn-krunker-changelog-versions.txt`).
  The 10.0.3, 10.0.2, 10.0.1 and 10.0.0 entries are copied verbatim in the last section. The text is
  Krunker's, not KrunkNative's.
- **Bundle diff (inferred from code).** The v10.0.3 client `game.js` (13.1 MB, single-line, obfuscated)
  and the v10.0.2 client `v10.0.2.js` (13.6 MB) were both run through webcrack (static, no
  execution): `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` and `deob/v10.0.2/game.deob.js`
  (decoder `deob/tools/deob.cjs`, see `deob/README.md`). The string literals, object keys, i18n keys
  and numeric literals of each build were extracted (`deob/tools/extract.js`) and compared as sets. Identifiers are mangled per build, so the diff
  is at the level of strings, keys and numbers, not of code. Items marked "inferred" below come from
  this diff and are not in the official text.

Not confirmed: the release notes are Krunker's; nothing here is a statement from Krunker beyond the
official text. Where a bundle item has no official line, it is marked "unlisted".

## What changed in the bundle (inferred from code)

| Area | Evidence in the 10.0.3 bundle | Official line |
|---|---|---|
| Quality setting `noVideoSkins` ("Disable Video Skins") | `settings.quality.noVideoSkins`, default off, needs restart; drops `video` (and `projection`, `texSrc`) from a skin texture that is not a user asset | Yes |
| Map setting `rampFix` ("Solid Ramp Sides", map settings "Other") | `Player.blockRampSide`; `!!map.rampFix` or `config.rampFix` | Yes |
| KrunkScript AI parts and hits | `preciseHits`, `setPartDamage`, `glowPart`, `setNodeVisible`, `getPartPosition`, `onAIPartDamage`, `bodyDamage`, `ai.solid`, `alwaysSync`, `loadModel` / `loadTris` / `dropTris` | Yes |
| KrunkScript player pose | `holdL` / `handL` attach, `handLAnchor`, `syncHandL`, split arms (`splitArm`, `setSplitArms`) | Yes |
| GLB materials | `reflections` / `getEnvMap` (opt-in), GLB checks: "not a GLB", "GLB has no JSON chunk", "model too large" | Yes (reflections); the checks are unlisted |
| Round and game state | `started` on `GAME.CONFIG.getMatch()`, `complexCollision` | Yes |
| Storage queue | `queueStorage`, `flushStorage`, "Storage busy", "Storage timeout", "Too many storage writes queued" | Unlisted (editor storage) |
| Purchase flow | `purchase.opening`, `purchase.confirming`, `purchase.confirmSlow`, `purchase.openFailed`, `purchaseWaitLabel`, `waiting` | Unlisted |
| Skin paint badge | `skinPaintBadge`, `skinPaintSwatch`, `windows.skins.painted`, a `format_paint` icon | Unlisted (10.0.2 said paint shows in Customize lists) |
| Renewal price text | `$9.99` and ` $4.99` strings appear as literals | Unlisted (10.0.2 has the $4.99 renewal price) |
| Profile picture upload | `pfpUploaded`, `setPfpUploaded`, `pfpUrl` | Unlisted |
| Asset host | `userAssetsBase` / `USER_ASSETS_BASE` added; the `https://assets.krunker.io/p` literal removed | Unlisted (infrastructure) |
| Version | `10.0.3` string; `10.0.1` removed; build hash in asset URLs changed | Yes (version) |
| Packet trailer `ahNum` step | `this.ahNum = add(this.ahNum, 3)` (`(a + b) & 255`) before every send but `cptR` (`deob-10.0.3.js:111514`, helper 27698); 10.0.2 added `1597` (& 255 = 61, `v10.0.2/game.deob.js:111476`) | Unlisted (wire) |

Removed in 10.0.3 (inferred, unlisted): the `Eterno Simulator` literal in the client, and the
`setTimeout` / `listeners` / `localInspectSound` helper names. The number-literal sets differ in four
values (`60000000`, `1179937895` added; `0.6075`, `1597` removed). `1597` was 10.0.2's `ahNum` step;
10.0.3 steps by 3 (row above; a set diff cannot show it, 3 is everywhere), which is KN's
`KRUNKER_AHKEY` 0x3 (`shared/include/krunker_build.h`). `1597334677`, in both builds, is a
`Math.imul` hash multiplier (`deob-10.0.3.js:59156`), not the key. `60000000` is the asset download
cap (204238), `1179937895` the GLB magic (204351) and `0.6075` a render-block y offset.

## Classification and KrunkNative action

Legend: **Ported** (in this change), **Not ported** (reason), **Bug fix, nothing to do** (KN does
not have the code), **Internal** (ignored), **Decision** (a feature KN lacks, left for a decision).

### General

| Official 10.0.3 line | Class | KN action |
|---|---|---|
| Added a Disable Video Skins setting (Quality) | Feature | **Not ported.** KN has no video skins: its own notes say the video texture is not ported (`client/src/player_mesh.c` TODO at the skin code, `zombies_render.c` note), so there is no video playback for the toggle to switch off. Adding the toggle now would be a control with no effect. Port it with video skins (see Decision). |
| Fixed paintable melees showing as the Combat Knife in the loadout | Bug | Not verified. KN's loadout melee icon path (`hud_ui.c`) uses the melee skin id; the loadout preview was not compared. |
| Fixed explosions sometimes not showing | Bug | Not verified. |
| Fixed a checkered texture on some skies and map assets | Bug | Not verified. |
| Fixed playing a map with spaces in its name from Social sending you to a public game | Bug | Not verified (`client/src/ui/social_ui.c`). |
| Fixed follower counts on posts not matching the profile | Bug | Backend (social); not verified. |
| Fixed players sometimes staying in the game as a copy after trading | Bug | Not verified. |
| Fixed a server crash that could end a game for everyone | Bug | Server code; the client bundle cannot show it. Not verified. |

### Market

| Official 10.0.3 line | Class | KN action |
|---|---|---|
| Fixed the item Stats tab showing no data at busy times | Bug | Not verified (`client/src/ui/v10_market.c`, backend). |

### Clans

| Official 10.0.3 line | Class | KN action |
|---|---|---|
| Recruitment minimum level and K/D now apply to every join request | Bug | **Ported.** The post's minimum K/D refuses every join request and approval: the Apply button (`client/src/ui/turf_ui.c`), the brain's `run_apply` (`services/brain/db_turf.c`, `recruit_kd_refused`), the join by name (`run_clan_request`) and the owner's approval (`run_clan_answer`, `services/brain/db_social.c`). K/D is the profile's: lifetime kills / max(deaths, 1) (`players.player_kills` / `player_deaths`; no row = 0). Refusal: "Requirements Not Met" (v10's disabled button). The minimum level is still checked on Apply only (KN's own gate). |
| Fixed recruitment pages jumping back to page 1 | Bug | Not verified (`client/src/ui/clan_ui.c`). |

### KrunkScript and Editor

| Official 10.0.3 line | Class | KN action |
|---|---|---|
| Added player.pose.holdL with a "handL" attach point | Feature (KrunkScript API) | Not ported: KN has no `player.pose` (no hold or split-arm API). |
| player.pose armL and armR now move only their own arm in third person | Bug (KrunkScript pose) | Not ported: KN has no `player.pose`. |
| Added the Solid Ramp Sides map setting (rampFix) | Feature (map and physics) | **Ported:** map flag `rampFix` (`shared/src/map.c`, `shared/include/shared.h`) and the ramp side block in `shared/src/player.c` (`movement_block_ramp_side`, the check in the ramp branch of `player_do_map_collisions`). Regression test `shared/tests/ramp_fix_test.c` (fails without the port). The editor checkbox (Essential panel, next to Force Old AO, `client/src/editor/editor_gameconfig.c`) and the game option `config.rampFix` (`GameConfig.ramp_fix`, folder Other, `shared/src/game_config.c`) are ported too; `ramp_fix_test` checks the config path. |
| Added preciseHits for AI (bodyDamage, setPartDamage, glowPart, setNodeVisible, getPartPosition, onAIPartDamage) | Feature (KrunkScript AI) | Not ported: KN's AI is one model with no named parts, and KN has no part hooks or `bodyDamage`. |
| Added ai.solid, a hold option for ai.playAnim and an alwaysSync spawn option for AI | Feature (KrunkScript AI) | Ported: `AI.spawn` `alwaysSync` (`GameAIData.always_sync`); the AI skips the 800 range of `game_ai_sync` (`shared/src/ai.c`). The official line only names the option; "every player whatever its distance" is read from the name. `ai.solid` and the `hold` option of `ai.playAnim` are not ported: neither exists in v10.0.2 (`~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js` has no `solid` in JavaScript form and no options object on `ai.playAnim`), and the official line gives no behaviour. v10.0.2's `AI.sync` (`this["sync"]` in v10.js) skips any AI more than 800 from a non-spectating player, sent or not; `alwaysSync` is KN's exemption from that gate, and v10.js has no `alwaysSync`. |
| Added opt-in environment reflections for GLB materials | Feature (rendering) | Not ported: KN has no GLB material reflections (env maps exist for skins only, `client/src/skin_fx.c`). |
| Live assets with complex collision now stop shots | Feature (rules) | No change. Shots trace complex-collision objects by their box (`combat_trace`, `shared/src/player.c`), so KN already stops shots at them. Not compared with the v10.0.2 bundle (not on disk). |
| GAME.CONFIG.getMatch().started is now false during the round countdown | Feature (KrunkScript) | Ported: `CONFIG.getMatch().started` (`shared/src/ks_game/ks_hostobj.c`) is false while a pre-match wait timer runs (the countdown) and true once the round has started. The official line does not define the exact state; this reads "countdown" as the non-contTime wait timers (`mode_match_starting`). v10.0.2's `getMatch` (v10.js) returns only `players`, `capacity` and `teams` (and `null` with no game), so `started` is 10.0.3-only and the reading is inferred. |
| AI models now load once per game | Internal (performance) | Ignored. |
| Fixed scene objects stacking up and looped sounds breaking at the next round | Bug (KrunkScript round restart) | Not applicable: the client unloads the script, with its scene objects and sounds, on every round (`ks_client_round` in `client/src/ks/ks_client_host.c`). |
| Fixed walking AI falling through the floor | Bug (KrunkScript AI) | Fixed in KN (`shared/src/ai.c`, last branch of `ai_collide_map`): an AI already inside a box's height range, more than 3 units under its top, matched no branch and fell forever. Reproduced by `ai_floor_test` (walker spawned 2 and 6 units under a flat floor: y to -100 before). A walker on the floor never fell in five normal cases. v10.0.2's collision rule is the same as KN's before the fix (`collidesAcc` block in `~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js`), so the fix is KN's own, not a v10.0.2 port. |

### Unlisted bundle items

| Item | Class | KN action |
|---|---|---|
| Purchase-progress strings (`purchase.*`, `purchaseWaitLabel`) | Feature (unlisted) | Done: the keys are in `ui/i18n/en.json` (10.0.3's English table) and `#purchaseWaitLabel` is in the page and `ui/css/main.css`. Nothing in `client/src/` writes the label yet. |
| Skin paint badge markup | Feature (unlisted) | **Ported:** `client/src/ui/skin_paint.c` (`skin_paint_badge`, 10.0.3's `skinPaintBadge` / `skinPaintSwatch` markup and the `windows.skins.painted` title, `deob-10.0.3.js:241529`), drawn on owned cards by `ui_loadout.c` and `v10_market.c`; test `client/tests/skin_paint_test.c`. |
| GLB validation messages, profile picture upload keys, storage queue | Internal or editor (unlisted) | Ignored. |
| Renewal price text | Feature (unlisted) | Decision (the $4.99 renewal is a 10.0.2 item). |
| Asset host | Internal | Ignored. |

## Ported in this change

- `rampFix` ("Solid Ramp Sides"): `shared/src/player.c`, `shared/src/map.c`, `shared/include/shared.h`.
  The behaviour is v10.0.3's `blockRampSide`, with `collision_padding` (1.1) as in v10. The
  `config.rampFix` game option is ported too (`GameConfig.ramp_fix`, folder Other).
- Changelog: the official 10.0.3 entry is the first update in `ui/windows/kn-versions.txt`, so
  What's New (`client/src/ui/v10_news.c`) shows 10.0.3. KN's 1.2.0 entry is kept below it.

## Decisions needed

1. Video skins (a larger feature, KN has none): the Disable Video Skins toggle only makes sense with
   them.
2. Clan join K/D gate: the source of K/D and the brain enforcement.
3. KrunkScript AI: `ai.solid` and the `hold` option of `ai.playAnim` exist in neither v10.0.2 (v10.js) nor the
   official 10.0.3 line, and KN's AI bodies do not block players, so there is no behaviour to port. A spec from
   Krunker is needed. KN's `ai.playAnim` now passes `reps` as v10.0.2 does (`game_ai_play_anim_reps`): the local copy plays the clip reps times; the `ana` packet carries no reps and networked clients play once, as v10.0.2's servAnim does. The AI
   parts (`preciseHits`, `setPartDamage`, ...) and `player.pose` are not in KN at all.
4. GLB environment reflections (KN has none).
5. Done: the Solid Ramp Sides editor checkbox and the `config.rampFix` game option.

## Official text (verbatim)

The text below is Krunker's `docs/versions.txt` for 10.0.0 to 10.0.3, copied without change.

```text
 == UPDATE 10.0.3 ==
 * General
 - Added a Disable Video Skins setting (Quality)
 - Fixed paintable melees showing as the Combat Knife in the loadout
 - Fixed explosions sometimes not showing
 - Fixed a checkered texture appearing on some skies and map assets
 - Fixed playing a map with spaces in its name from Social sending you to a public game
 - Fixed follower counts on posts not matching the profile
 - Fixed players sometimes staying in the game as a copy after trading
 - Fixed a server crash that could end a game for everyone in it
 * Market
 - Fixed the item Stats tab showing no data at busy times
 * Clans
 - Recruitment minimum level and K/D now apply to every join request
 - Fixed recruitment pages jumping back to page 1
 * KrunkScript & Editor
 - Added player.pose.holdL with a "handL" attach point, to hold an item in the left hand while keeping the weapon
 - player.pose armL and armR now move only their own arm in third person
 - Added the Solid Ramp Sides map setting (rampFix): a ramp's high side blocks players like a wall
 - Added preciseHits for AI: shots hit the animated model, with bodyDamage, setPartDamage, glowPart, setNodeVisible, getPartPosition and onAIPartDamage
 - Added ai.solid, a hold option for ai.playAnim and an alwaysSync spawn option for AI
 - Added opt-in environment reflections for GLB materials
 - Live assets with complex collision now stop shots
 - GAME.CONFIG.getMatch().started is now false during the round countdown
 - AI models now load once per game
 - Fixed scene objects stacking up and looped sounds breaking when a map starts its next round
 - Fixed walking AI falling through the floor

 == UPDATE 10.0.2 ==
 * General
 - Added a Show Only Paintable filter to the Items catalog
 - Painted items now show their paint in the Customize skin lists
 - Items on a creator's profile now open in the profile view
 - The Universal Starter Ticket no longer drops from Starter and Elite spins
 - Bundles and Premium packs can now be bought with the new payment option, including as gifts
 - Renewing a username you already own now costs $4.99 (new usernames stay $9.99)
 - Fixed some reflective skins rendering dark in game
 - Fixed thrown melee weapons not exploding or showing
 - Fixed profile links for players with an apostrophe in their name
 - Fixed a new premium profile picture not showing right away
 - Fixed unfavoriting some older favorites
 - Other minor UI fixes
 * Market
 - Fixed collectibles price floor showing incorrectly for listing
 - Fixed the Black Market's helpful reports requirement
 * Ranked
 - Eterno Simulator left the ranked pool
 - Retired the Bureau ranked challenges; completions and rewards already earned stay
 * Bastion
 - Taller hitboxes for the Catacombs twins
 - Keys are easier to pick up
 - Some breakable objects can now be shot, not just hit with melee
 - Moved an archer tower, plus more small layout and collision tweaks
 * Studios & Editor
 - Triggers can now have up to 8 actions
 - Fixed uploading mods from the game, and Premium players get the larger mod (50 MB) and thumbnail (500 KB) limits
 - Map storage saves made quickly one after another are no longer lost
 * Developer API
 - Added GET /api/events with map featured and unfeatured events
 - Added webhooks for those events, set up in the partner portal
 - Added GET /api/maps/featured

 == UPDATE 10.0.1 ==
 * General
 - Added Discord account linking in Account > Connections, with an optional Join Server button
 - Added a Show Findings chat setting: "found" item messages no longer follow Show Unboxings
 - Improved performance when textures, new skins and weapons load in
 - Fixed first-time visitors getting stuck on the loading screen
 - Fixed see-through walls
 - Fixed rainbow wireframe skins turning black
 - Fixed the muzzle flash drifting off the barrel
 - Fixed projectile orbs vanishing behind the map
 - Fixed scrolling and rotating textures not animating on still objects
 * Studios & Editor
 - Studio members are now invited and can accept or decline, and owners can transfer ownership
 - Studios can apply for monetization from the Games tab
 - Scripts use the skin id shown on the studio Skins tab, and studio maps load all of the studio's active skins
 - Republishing a map without a preset now keeps its saved preset
 - Fixed the Editor freezing when adding an AI spawner
 - Fixed duplicated objects losing their texture
 - Fixed obj.texture throwing before the texture loads
 - Fixed dyes not applying or showing their Shader Studio graph when tested in the Skin Manager
 * Ranked
 - The ranked pool now rotates Undergrowth, Industry, Site, Bureau, Eterno Simulator and Frontier, with Sandstorm always in
 - Burg, Shipment, Lumber, Atomic and Lush left the ranked pool
 * Bastion
 - Smaller Hydra head hitboxes, and the heads were moved so fireballs no longer hit the wrong spot
 - The King's shot is slower and smaller, with a bigger visual
 - Archer skeletons have a longer melee range, and Archer and melee skeletons have taller hitboxes
 - Fixed getting out of bounds when entering the Throne
 - Added a safety floor below the Catacombs and Throne arenas
 - Fixed a missing face at the Basin and z-fighting on the throne sword and rugs
 - Lowered some dirt in the Catacombs, and a Throne block no longer blocks enemies
 - Letters and other random pickups now spawn in solo and duo runs
 - Hosted raids now count end-of-run challenges and roll random pickups
 - Beating the raid without taking damage, on 1 HP or without firing a gun now requires reaching the Throne

 == UPDATE 10.0.0 ==
 * General
 - Added 166 new items across Spins, Battlepass, Ranked, Eterno Shop, Black Market, Twitch drops and the Bastion raid
 - Added 55 new spin items, including the V-CORE set: equip 2 of its pieces for a shock effect
 - Added new Season 10 Battlepass with 80 levels and 30 new items
 - Added 5 new Twitch drops
 - Added new Season 10 Eterno Shop with 10 new items, including the new Eterno sniper
 - The Season 9 Eterno AK is now tradable
 - Added the Krunkuku Tamer to the Black Market (requires all 7 Krunkuku charms)
 - Added Universal Starter, Elite and Spin Tickets: they always spin the current season's wheels
 - Added Season 10 badges, including the Season 10 Contributor badge in the store
 - Redesigned the main menu slightly
 - Merged Class, Loadout and Customize into a single window with a live character preview as before
 - Added Titles: equip a title to show on the scoreboards and your profile
 - Added Kounters: weapon and melee skins with a built-in counter that tracks kills, headshots, shots and more for that exact item
 - Added ability to buy extra usernames and switch between them once every 7 days
 - Owned usernames last a year and can be renewed from the usernames popup
 - Added opt-in for news and promotional emails in Account Settings
 - Added gifting: send Eterno shop items (KR or real money) and the one-off premium packs to other players
 - Added new payment option for KR, store items, gifts
 - Emperor's Blade can now be claimed from Kracked in the Black Market in any ranked season, and grants the No-Life title
 - Added the reflective Gold, Silver and Prism paints and the Codefall paint
 - Added Gold Paint to the Paint Wheel
 - Item details can now preview a paint together with wear
 - Prize Wheels: Starter and Heroic get bigger tiles, tickets still possible to use from View Tickets button
 - Added a popular custom games row to the main menu
 - Added Studios: a team that shares your games, with up to 20 members
 - Added version history to maps, with the ability to revert and to restore deleted games
 - Added custom currencies and a scripted economy for Studios
 - Added custom skins to games, equippable while playing that game
 - Added economy analytics for Studio owners
 - Studios keep 90% of the KR players spend in their games
 - Added a confirmation prompt before spending KR in a custom game
 - Skin makers can now author shaders as node graphs in the Skin Manager (replaces custom GLSL)
 - Changed the Skin Manager to full screen, with a paged list and item thumbnails
 - Added shader graph rendering to the skin Test button
 - Added a charm placement tool and custom swap and inspect sounds to the Skin Manager
 - Added a new "WebGPU rendering" setting
 - Updated three.js and moved the game, viewer, editor and menu previews to the new renderer
 - Improved performance for players, their cosmetics and particles
 - Improved performance on large maps
 - Various server stability and security fixes
 * KrunkScript & Editor
 - Added Entities: replicated objects with drivable vehicles, seats, animated model parts and triggers (WIP)
 - Added an archetype browser and isolated part editing to the Editor
 - Added entity counters and undo to entity authoring
 - Added GAME.ENTITIES, player.pose, setSkin, getSkin, TIME.freeze, TIME.unfreeze, player.aiKills and player.godMode
 - Added GAME.SCENE.addParticles, RAYCAST.from, RAYCAST.fromPlayer, player.pose.hold with a "hands" attach point, player.speed and CONFIG.getMatch()
 - Added AI damage and death hooks, script control of AI (target, moveTo, team) and a hideTag spawn option
 - Added damage and destroyed hooks for destructible map objects
 - Script lights can now change intensity, distance and color live, and SCENE.setFog takes a near distance
 - Added a Player Damage game setting for co-op maps
 - Model materials can opt out of fog, and maps can set a shadow bias to remove shadow stripes on large floors
 - Added KrunkScript samples (3D Pong, 2D Car Game, Basic Collisions and more) and brought the default maps back to Templates
 - Updated the KrunkScript docs with examples and a what's new guide
 * Maps
 - Added Bastion, a new raid with 39 items to collect from its bosses and secrets
 - Bastion's Contraband and Unobtainable drops are tradable, apart from Aureon's Armour set
 - Added Piazza and Barnyard to public matches
 * Challenges
 - Reworked the Nightmare challenges into lifetime ladders for nukes, kills and wins
 - Added challenges for Erupt, HQ, Vivo, Eterno Simulator, Tropicano and Frontier, plus a "Complete all" challenge for every map and raid
 - Added boss and enemy challenges to every raid, and new secrets to find on Soul Sanctum, Laboratory, Khepri and Bastion
 - Challenges can now reward titles and ranked points, and challenges you already completed grant their new title automatically
 * Ranked
 - Added 19 new items to the Ranked shop
 - Added Prism Paint to the Ranked shop
 - Changed the ranked map pool to rotate every season (Sandstorm plus 4 maps from the competitive pool)
 - Added Ranked Challenges that refresh every season, in the challenges window and the ranked menu
 - Fixed a match failing right after being found and counting you as a no-show
 * Bug Fixes
 - Fixed moved objects knocking another object out of the collision grid
 - Fixed dynamic objects with collision turned off still blocking shots
 - Fixed Turf Wars banners not being replaced or removed on update
 - Fixed trade previews not showing the exact copy's wear, serial and pattern, and added paint and mint badges to trades
 - Fixed item tooltips showing behind trade windows
 - Fixed map leaderboards only showing one board on maps with both a score and a time leaderboard
 - Fixed asset thumbnails and sound previews in the profile Assets tab
 - Fixed the lost connection message showing behind the menu
 - Fixed the daily spin wheel showing 0 tickets until you reloaded
 - Fixed gifting an item the player already owns not saying why it failed
```
