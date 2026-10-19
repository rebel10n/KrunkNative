# Krunker Strike maps

Krunker Strike rotates seven maps: Burg, Sandstorm, Subzero, Lostworld, Oasis, SkyTemple and Habitat.
Each one is Krunker v10.0.2's official map of that name, and the Strike flavor loads the same v10 file the
base catalogue loads (`assets/maps/official/NN_<name>.json`, unmodified). Code: `shared/include/strike_maps.h`,
`shared/src/rules/strike/maps_strike.c`, and the two flavor hooks in `shared/src/map_catalog.c`. Test:
`strike_maps_test`.

## The pool

| key | name | v10.0.2 file | catalogue index (= v10 mapIndex) | objects | spawns |
|---|---|---|---|---|---|
| burg | Burg | 00_burg | 0 | 1627 | 9 |
| sandstorm | Sandstorm | 02_sandstorm | 2 | 1362 | 9 |
| subzero | Subzero | 03_subzero | 3 | 1288 | 12 |
| lostworld | Lostworld | 07_lostworld | 7 | 1336 | 14 |
| oasis | Oasis | 09_oasis | 9 | 1338 | 8 |
| skytemple | SkyTemple | 15_skytemple | 15 | 1351 | 8 |
| habitat | Habitat | 22_habitat | 22 | 1374 | 8 |

Object counts are the raw `objects` arrays. The v10 list (`shared/include/krunker_maps.h`, pinned by sha256
in `tools/krunker/maps.lock.json`) names one file per map, so each pool map's latest version is that file;
v10 has no older revision of any of them. The names and the order are FRVR Krunker Strike's (the mapPool
names of build 1.37.2, rotated as mapWeight copies then a uniform pick). Every weight is 1, because
FRVR's server values (`/v1/game-config`) are not known.

`bot_ftue_v2` is the exception. It is Strike's copy of the controls tutorial's sandbox (647 objects, FRVR
1.37.2's `maps/bot_ftue_v2.json`, `assets/maps/strike/bot_ftue_v2.json`). Krunker v10.0.2 has no official file
for it (the v10 bundle names it only in an `isBotFTUE` branch and the official list does not include it),
so it stays where it is. It is outside the pool and the rotation (weight 0, no catalogue index).

## How the Strike flavor plays them

Both hooks in `shared/src/map_catalog.c` are gated on `flavor_get() == FLAVOR_STRIKE`; FLAVOR_BASE is
unchanged.

- `catalog_path()` returns `StrikeMap.path` for a pool map. The pool path is the v10 file, the same path the
  base catalogue entry names, so both flavors load identical map data. The sandbox is the only Strike-only
  file. The catalogue caches each map once per process, so the flavor must be set before the first load;
  server and client both do this at the top of `main()`.
- `map_catalog_rotation()` returns the pool's catalogue indices in FRVR's order.

The rotation feeds the server's custom-map-dir branch (`server/src/config.c`) and `game_configure()` without a
map list (`shared/src/game.c`, `map_list_with_custom` in `shared/src/map.c`). Krunker's own rotation
(`g_rotation_maps`) stays under FLAVOR_BASE.

The brain links `map_catalog.c` on its own, so `services/brain/CMakeLists.txt` also builds `flavor.c` and
`maps_strike.c`. The brain never sets a flavor, so its catalogue stays base.

Thumbnails: Strike's page shows map names only, and the pool carries no thumbnails.

## History: FRVR's copies (retired)

Until the switch to v10 files, the pool played FRVR 1.37.2's own copies under `assets/maps/strike/<key>.json`.
They were not v10 files: they were v7-era maps (or the first Habitat and SkyTemple) as FRVR last exported
them, with 1192 to 1604 objects. Their object and spawn counts were Burg 1604 / 9, Sandstorm 1339 / 9, Subzero
1272 / 12, Lostworld 1328 / 14, Oasis 1333 / 8, SkyTemple 1192 / 8, and Habitat 1261 / 8. The 2026-10-14 nav and
playtest measurements (`doc/strike/parity.md`) were taken on those copies and do not apply to the v10 files.
The bot fixes for the SkyTemple ledge (`bot_path_guard` in `shared/src/bot_path.c`) apply to either file.
`git log -- assets/maps/strike` finds the commit that removed the seven copies.

## Verification

- `strike_maps_test` (182 checks): the table, each pool path equal to the base catalogue entry's path,
  each v10 file parses and passes `map_validate_error`, `map_init` keeps every object and prefab id and
  builds the spawn count, the Strike rotation and `game_init()` on every pool map, the FTUE sandbox entry.
- `map_drop_test`: 83 catalogue maps and 46 official maps, 0 dropped objects.
- `bot_test`: the island nav runs use the v10 SkyTemple (`skytemple_v2`); the separate Strike SkyTemple
  entry was removed as a duplicate of that file.
- Headless client (`client_bin --flavor strike --offline`): each of the seven v10 files loads with
  `Loaded map <name>`.
- Headless server (`server_bin --flavor strike`): rcon `map` switches to each pool map by name and the
  server lists it as current, with no error lines in the log.
- `server/tests/bot_nav_e2e.py`: 49 of 49 on a repeat run, Strike's SkyTemple included. The first run had two
  base-flavor checks miss their thresholds (Kanji goals, Industry unreachable goals) on maps and code this change
  leaves alone; the repeat passed them, so those are timing variance.
- `strike_flavor_e2e.py` (22 checks), `make test` and `make check`.
