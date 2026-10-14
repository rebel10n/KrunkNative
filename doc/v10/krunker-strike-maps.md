# Krunker Strike maps (S8 strike-maps)

Krunker Strike rotates seven maps: Burg, Sandstorm, Subzero, Lostworld, Oasis, SkyTemple and Habitat.
This note records where FRVR's map files come from, how they differ from the copies the base game ships in
`assets/maps/`, and how the Strike flavor plays them. Code: `shared/include/strike_maps.h`,
`shared/src/rules/strike/maps_strike.c` (S1 strike-rules owns them from now on), and the two flavor hooks
in `shared/src/map_catalog.c`. Test: `strike_maps_test`.

## Sources

**Primary: FRVR Krunker Strike build 1.37.2, `/home/user/src/KrunkerStrike_v1.37.2/`.** This is a local
snapshot from 2026-06-13 (git 06b47ff). 1.37.2 is the build the Discord Activity still served on
2026-10-11. Read it as text only and never execute it.

- `public/assets/index-Cz2uYXJQ.deob.js` holds the map table `xh` (`{name, fileName}`, in Krunker v7
  `src/data/maps.js` order, 32 entries plus `bot_ftue_v2`, `mini_industry` and `subzero_xmas`).
- Round setup (`this.init`) reads `gameConfig.remoteFeatures.mapPool.prod` (`{mapName, mapWeight}`). It
  looks each entry up by `name` in `xh`, pushes its index `mapWeight` times into `config.maps`, and the
  client then fetches `"maps/" + fileName + ".json?v=1"`.
- The pool and its weights live on FRVR's server (`/v1/game-config`), which we have not fetched. The seven
  names are the Strike pool. Every weight is 1 until that data exists.
- `public/maps/*.json` are the map files.

**Older generation: the Internet Archive, 2025-07-22.** Mirrored in `~/.cache/kn-strike/live/maps/`, whose
`README.md` lists sha256 values for both the raw gzip body and the decoded JSON. The source URL is
`https://web.archive.org/web/20250722id_/https://1011683823555199066.discordsays.com/.proxy/maps/<name>.json`,
for burg, habitat, lostworld, oasis, sandstorm, skytemple and subzero.

- These plain-named files belong to an older Strike client. The 1.37.2 bundle never names them.
- Burg, Lostworld, Oasis, Sandstorm and Subzero there are the v3.1.1-era maps. Their objects overlap our
  `311_<name>.json` copies by 96-99% (prefab, position, size).
- `habitat.json`, `skytemple.json` and `oasis.json` in the archive are byte-identical to 1.37.2's files of
  those names. 1.37.2 does not use `oasis.json`; it plays `oasis_v3`.

**Shipped:** the 1.37.2 files are the Strike versions, unmodified, under `assets/maps/strike/<key>.json`.

| key | FRVR name | FRVR 1.37.2 file | catalogue index | base file it stands in for | sha256 (first 12) | objects | spawns |
|---|---|---|---|---|---|---|---|
| burg | Burg | burg_new | 0 | burg_new | 66452866a84d | 1604 | 9 |
| sandstorm | Sandstorm | sandstorm_v3 | 2 | sandstorm_v3 | 46d09626627e | 1339 | 9 |
| subzero | Subzero | subzero_new | 3 | subzero_new | 22fd5649856f | 1272 | 12 |
| lostworld | Lostworld | lostworld_s6 | 7 | lostworld_s6 | 93b379796ba0 | 1328 | 14 |
| oasis | Oasis | oasis_v3 | 9 | oasis_v3 | dcca47e2b387 | 1333 | 8 |
| skytemple | SkyTemple | skytemple | 15 | skytemple_v2 | 8e33e4e20413 | 1192 | 8 |
| habitat | Habitat | habitat | 22 | habitat_v2 | e917281a6f93 | 1261 | 8 |

FRVR's table index equals the v7 mapIndex, which is also our catalogue index. That is why the pool maps
straight onto `map_catalog.c`.

## Differences from our copies (1.37.2 vs `assets/maps/`, which are v7.1.4's files)

Geometry was compared as a multiset of (prefab id, position, size), with pooled `si` sizes resolved.

- **Burg** (`burg_new`): 1598 objects match. Ours has 29 that FRVR's lacks: 13 AI_SPAWNER (55), 7
  SERVER_NODE (82), 6 MAP_ANCHOR (96), 2 RAMP, 1 TILES. FRVR's has 6 of its own: 4 CUBE, 1 TILES, 1 RAMP.
  `config.modes` goes from `[0,2,19,27]` to `[0,1,2,3,14,19,21,25,28,29,31,32,34]`. `botPaths` is added
  (3 paths). `mmId` is dropped.
- **Sandstorm** (`sandstorm_v3`): 1336 match. Ours has 26 extra (8 AI_SPAWNER, 8 CUSTOM_ASSET, 6
  SERVER_NODE, 4 MAP_ANCHOR); FRVR's has 3 extra CUBE. `config.modes` goes from
  `[0,1,2,10,14,19,25,27,29,31,32]` to `[0,2,10,14,19,25,29,31,32,34]`. `botPaths` is added (4). `mmId`
  is dropped.
- **Subzero** (`subzero_new`): 1270 match. Ours has 18 extra (6 MAP_ANCHOR, 6 SERVER_NODE, 3
  CUSTOM_ASSET, 2 CUBE, 1 TRIGGER); FRVR's has 2 CUBE. `config.modes` goes from
  `[0,1,2,10,14,19,25,27,28,29,31,32]` to `[0,2,10,14,19,25,28,29,31,32,34]`. Our KrunkScript (`scripts`)
  is absent in FRVR's file. `botPaths` is added (4). `mmId` is dropped.
- **Lostworld** (`lostworld_s6`): all 1328 FRVR objects match. Ours has 8 extra (5 MAP_ANCHOR, 2
  CUSTOM_ASSET, 1 TRIGGER). `config` gains `classes: [0..9, 11, 12, 13]` and `settings.maxPlayers: 8`;
  `modes` goes from `[0,2,3,27]` to `[0,2,3]`. There is no KrunkScript and no `botPaths`.
- **Oasis** (`oasis_v3`): all 1333 match. Ours has 5 extra (4 MAP_ANCHOR, 1 RAMP). `config.modes` gains
  34. `botPaths` is added (6).
- **SkyTemple**: a different map. FRVR plays the first SkyTemple (2021, v0.1 in its `mapData` table), not
  v7's `skytemple_v2`, and no geometry is shared.
  - The world is lower: floors at y -1 to 18 and `dthY` -20, against ours at about 150 with `dthY` 115.
  - The sky is lighter: `sky`/`fog`/`ambient` 13559295, `fogD` 1165, and new dome colours.
  - The 8 spawns are different. `config.modes` is `[0,3,19,27,28]`.
  - It has 6 "pickup Junk" economy triggers (FRVR's Junk currency; `start_destroyed`, 25% spawn chance).
- **Habitat**: a different map. FRVR plays the first Habitat, not `habitat_v2`, and no geometry is shared.
  - It is a night map: `light` #ffffff at `lightI` 0.1, `ambient` 5002621 at `ambientI` 1.5, fog
    3949409, sky dome #3C4361.
  - Spawns sit about 200 units further along +x. `config.modes` is `[0,2,19]`, and there are 12
    `botPaths`.
  - 5 CUSTOM_ASSET objects carry `ms` (model_size) 0.02-0.04, below the editor's range of 0.1-100. This
    is a warning only.

Things all seven share:

- None contains objects newer than v7.1.4. FRVR's copies predate Krunker adding MAP_ANCHOR, SERVER_NODE
  and AI_SPAWNER to these maps.
- None has `mmId`. Most gain FRVR `botPaths`, which our loader keeps as unknown data and does not use.
- Mode ids are v7 mode indices (`shared/include/modes.h`). 34 shows up only in FRVR's lists.

## How the Strike flavor plays them

Both hooks in `shared/src/map_catalog.c` are gated on `flavor_get() == FLAVOR_STRIKE`; FLAVOR_BASE is
unchanged.

- `catalog_path()` returns `StrikeMap.path` for a pool map. `map_catalog_json()`, the web prefetch and
  `map_catalog_load_file()` therefore load FRVR's file wherever that catalogue index is used, including
  in the base rotation. The catalogue caches each map once per process, so the flavor must be set before
  the first load. Server and client both do this at the top of `main()`.
- `map_catalog_rotation()` returns the pool's catalogue indices in FRVR's order.

That rotation currently feeds only the server's custom-map-dir branch (`server/src/config.c`).
`game_configure()` without a map list still uses its own `g_rotation_maps` copy of v7's rotation
(`shared/src/game.c` and `map.c`). Restricting a default Strike game to the pool is an S2 strike-gameplay
change: there, read `map_catalog_rotation()` under Strike.

The brain links `map_catalog.c` on its own, so `services/brain/CMakeLists.txt` now also builds `flavor.c`
and `maps_strike.c`. The brain never sets a flavor, so its catalogue stays base.

Thumbnails: FRVR's Strike page shows no map images; it uses map names only. None ship. Renders exist at
`~/.cache/kn-strike/s8-thumbs/<key>.png` (200x80, from the files above) if a Strike page later wants them.

FRVR CDN assets: the six Strike class icons (`textures/classes/icon_{0,1,2,4,5,7}.png`) and those classes'
weapon previews (`textures/previews/weapons/weapon_{1..6,9}.png`, from
`cdn.frvr.com/games/krunkerfrvr/...?build=1.37.2`, mirrored in `~/.cache/kn-strike/live/cdn/`) are
byte-identical to the Krunker files already in `ui/textures/`. Nothing new needs shipping for them.

## Validation and playtest (2026-10-14, the editor MCP on the shipped files)

v7.1.4 `validateMap` accepts all seven maps, and `map_validate`'s schema, trigger and limit checks report
no errors on any of them. Its only errors are nav-grid reachability ones, listed below. Every warning is either `trigger.event_not_native`
(`onInteractKey`, the Junk pickups) or a `bad_value` the v7 data already carries (`team` -1, `health`
50000, small `model_size`).

| map | validate | nav reach | 30 s, 6 bots, ffa |
|---|---|---|---|
| Burg | ok | 9/9 | 6 kills, 0 falls |
| Habitat | ok | 8/8 | 3 kills, 0 falls |
| Lostworld | 3 reachability errors | 12/14 | 11 kills, 11 falls into the water (death height y 5) |
| Oasis | 1 reachability error | 7/8 | 10 kills, 0 falls |
| Sandstorm | ok | 9/9 | 3 kills, 0 falls |
| SkyTemple | 1 reachability error | 7/8 | 0 kills, 227 falls |
| Subzero | ok | 12/12 | 5 kills, 6 falls |

The reachability errors are nav-grid false positives. Our base `lostworld_s6` and `oasis_v3` report the
same ones. A scripted walker (playtest `path`) leaves every flagged spawn:

- Lostworld spawn 0 goes down the invisible RAMP 55 to the y 41 ground (4/4 waypoints).
- Lostworld spawn 6 goes down RAMP 151 (5/5).
- Lostworld spawn 10 starts 1 unit inside floor 447 and walks off it (`spawn.inside_solid`).
- Oasis spawn 4 goes down RAMP 71 (4/4).
- SkyTemple spawn 4 walks the x < -242 strip and steps down 4 units onto the main ground (4/4).

The nav grid misses these unrotated invisible ramps and that step.

SkyTemple's bot falls come from the bot AI, not the map. Spawns 0 and 1 sit on a grass ledge between the
front house and the island edge, and bots walk off it: one bot alone falls 6 times in 10 s. v7's
`skytemple_v2` also loses bots off ledges (17 falls in 30 s). This belongs with the bot-nav owner
(edge/drop awareness on sky maps).

`strike_maps_test` (run from the repo root) checks:

- the table: order, keys, names, catalogue indices and lookups;
- each file: it parses, passes `map_validate_error`, `map_init` keeps every object and prefab id, and the
  spawn counts match;
- base: the 24-map v7 rotation and `habitat_v2`;
- Strike: the pool rotation, all seven files loaded through the catalogue, a non-pool map unchanged, and
  one `game_init()` per pool map.
