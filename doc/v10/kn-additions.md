# KrunkNative additions on top of v10.0.3

Krunker Box Office plays Krunker v10.0.3 (`target.md`). A few things are KrunkNative's own, added on
top of v10 on purpose. They are mods, not parity gaps: a parity audit compares KN with
`deob-10.0.3.js` and leaves these out. Each one is also marked at its definition in the code.

## The Nettspend class

The Nettspend is not part of v10's class table. v10.0.3's `classConfig` has 16 classes, Triggerman
to Infiltrator (`deob-10.0.3.js:115405-115747`, `deob/tables/cls-1003.json`); that is `CLASS_COUNT`
(16). KrunkNative's classes come after every v10 class (user, 2026-10-24: "the id can be changed to
always be last, it's a mod on top of v10"):

| macro (`shared/include/shared.h`) | value | meaning |
|---|---|---|
| `CLASS_COUNT` | 16 | v10's classes, ids 0-15: menus, challenges, class stats, `classLim0..15` (`GAME_CONFIG_CLASS_LIMITS`) |
| `KN_CLASS_BASE` | `CLASS_COUNT` | the first KrunkNative class id |
| `CLASS_ID_NETTSPEND` | `KN_CLASS_BASE + 0` (16) | the Nettspend, the last class |
| `CLASS_TABLE_COUNT` | `KN_CLASS_BASE + KN_CLASS_COUNT` (17) | the ruleset's table (`rules_class_count()`, `Game.class_count`) |
| `KN_CLASS_NETTSPEND_STORED` | 16 | the id that saved data holds |

This works like `WEAPON_ID_KN_BASE` (64) for the KN weapons, except that the class base follows v10's
table. If v10 adds a class, the Nettspend's id moves up with it and never overlaps a v10 class. The
table is `g_classes_v10` (`shared/src/rules/v10/classes_v10.c`): v10's 16 in order, then
`[CLASS_ID_NETTSPEND] = nettspend`. The code tells KrunkNative's classes from v10's with
`>= KN_CLASS_BASE`: the official-server gate `game_class_allowed`, `game_class_listed`
(`shared/src/game_config.c`), `comp_lobby_class_limit` (no classLim, so the team's size) and the v10 menu
avatar (`client/src/ui/v10_menu.c`). The class design (loadout, gates, comp, bots) is in
`doc/v7/decisions.md` "Nettspend class", and its balance in `gameplay.md` "Nettspend balance".

The id is stored as a number in these places:

| where | what |
|---|---|
| client html storage `classindex` (`client/src/ui/menu_ui.c`) | the menu's class |
| loadout presets `classindex` (`client/src/ui/ui_loadout.c`) | a saved preset's class |
| brain per-class stats `c<id>` (`services/brain/db_accounts.c`) | the account's class XP / stats |
| `match_results.class_id` (`db_accounts.c`) | match history |
| account `class` / `classIndex` (`services/brain/accounts.c`) | the account's last class |
| `mm_ranked_members.class_index` (`services/brain/db_mm.c`) | a ranked queue entry (short-lived) |
| `ui/textures/classes/icon_16.png` (`tools/nettspend/icons.py`) | `icon_index` = the id |
| wire: SPAWN / ENT class_index, comp lobby `c`, GAME_CONFIG `classes` | both ends build from one table |

The id has been 16 since 2026-10-14, and v10.0.3 still has 16 classes, so nothing is migrated today.
If v10's table grows, `CLASS_ID_NETTSPEND` no longer equals `KN_CLASS_NETTSPEND_STORED` and a
`_Static_assert` in `classes_v10.c` stops the build. To fix it, migrate the stored rows and
settings (16 becomes the new id; a stored 16 then means the new v10 class), rename the icon, and
update `KN_CLASS_NETTSPEND_STORED`.

Tests: `ruleset_test`, `nettspend_test` and `class_spawn_test` check that the Nettspend is the last
class at `KN_CLASS_BASE` and that the stored id still resolves to it. `config_test` checks that
GAME_CONFIG `classes` accepts it and refuses the id one past it. The class table comparison with
Krunker's (`weapons_v10_test`, against the v10.0.3 fixtures in `shared/tests/data/v10/`) covers only the
first `CLASS_COUNT` classes.
`client/tests/nettspend_loadout_e2e.py` reads the id from `shared.h`.

## The v3.1.1 and v1.5.2-era maps (`311_*`, `152_*`)

v10's catalogue has none of these maps. KrunkNative keeps them on purpose (user, 2026-10-12; and
2026-10-24: "old maps should be preserved"):

- `311_<key>`: v3.1.1's 15 official maps, shown as "Burg (3.1.1)" (`assets/maps/311_*.json`,
  thumbnails `ui/img/maps/v311/`);
- `152_<key>`: v3.1.1's 9 `custom_maps/old_*.json` maps from the v1.5.2 era, shown as "Burg (1.5.2)"
  (`assets/maps/152_*.json`).

They are listed in `shared/src/map_catalog.c` (`V311` / `V152`) as community maps
(`MAP_SOURCE_CUSTOM`, mapIndex -1). They are not in the rotation or in Krunker's official list, and
the old keys `v3_<key>` / `old_<key>` still resolve to them. They are map content, not code for an
older version. AGENTS.md's v10-only rule (no version switches, no older pages or rulesets) does not
retire them, and they load through the v3.1.1 / v7.1.4 map JSON compatibility that AGENTS.md
requires. `map_catalog_test` loads every one of them. The audit row
`deob/audit-2026-10-24/v1.5-audit.md` #9 ("retained extra: policy conflict") is settled: keep them.
