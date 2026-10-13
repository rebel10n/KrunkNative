# Runtime version toggle (v3.1.1 / v7.1.4 / v10.0.2): feasibility

The user has asked for this toggle explicitly (2026-10-13). It overrides AGENTS.md and
doc/v7/decisions.md "no v3.1.1 compatibility mode", which have to be updated when the toggle lands.
Repo state checked: origin/main 68bfdbb; v3.1.1 port on branch `origin/v3.1.1` (90cdea1, the merge base).
`git diff --stat origin/v3.1.1 HEAD` touches 3 120 files (+322 735 / -44 217), and 864 of them are under ui/.

## What already supports per-game rulesets

| Thing | Current state | Toggle impact |
|---|---|---|
| Weapons | `Game.weapons` / `weapon_count` (shared/include/shared.h:1816-1824), backed by the global `g_weapons[WEAPON_TABLE_COUNT]` (shared/src/weapons/weapons.c:6); 30 files index `g_weapons[` directly | the per-game pointer exists. The 30 direct users must go through `game->weapons` |
| Classes | `Game.classes` / `class_count`; global `g_classes[CLASS_COUNT 16]`; 15 files index `g_classes[` | same |
| Maps / modes | `Game.maps` (`const cJSON **`), `Game.modes`; `g_maps[32]`, `g_rotation_maps[24]` | per-version catalogues (v3.1.1 maps are already kept as `311_<name>`, decisions.md "Official maps") |
| Movement/physics | `const GameConstants game_constants` (shared/src/constants.c:12), a global used in 43 files (266 references) | the main refactor: make it `const GameConstants *` on `Game` (or a thread-local current pointer) |
| Protocol | `NET_PROTOCOL_MINOR` 3 (`_V7`), `PATCH` 1, negotiated per server from INIT (shared.h:2189-2201) | the version must be part of HELLO/INIT so client and server agree; peers of another ruleset are refused, never mixed |
| UI page | `html_ui_load_page(page)` already switches documents inside `ui_dir` (client/src/html/html_ui.c:111) | one page tree per version is possible without engine changes |
| v3.1.1 values | v3.1.1 constants.c differs from v7 only in `border_height` 250 vs 500 (`git diff origin/v3.1.1 HEAD -- shared/src/constants.c`); weapons (19 vs 29+KN), classes, map parsing and modes differ a lot | |

## What must be per version

| Area | v3.1.1 | v7.1.4 (current main) | v10.0.2 | Per-version mechanism |
|---|---|---|---|---|
| GameConstants | border 250 | border 500 | same as v7.1.4 (gameplay.md); differences are hitboxes (posed OBBs, `hurtboxScale`) and light units (x PI) | one `GameConstants` table per version |
| Weapons | 19 (`WEAPON_COUNT 19`, v3.1.1 weapons.h:159) | 29 + KN 29-31 | see gameplay.md | one `const Weapon *table[]` per version; KN additions (Tool Gun, Piss, Physics Gun) appended to each |
| Classes | v3 set | 16 | see gameplay.md | per-version `ClassConfig[]` |
| Modes | v3 modes | v7 modes (modes_v7.h) | v10 modes (gameplay.md) | per-version mode table; mode ids are not stable across versions |
| Map parsing | v3 field meanings (e.g. `pe` truthy flag, `obi`, singleUse) | v7 (`pe` 0..1 fraction, `obl`, map.c:416-485) | v10 (map-format.md) | a `MapDialect` argument to the parser. Today map.c already reads v3 JSON the way v7 reads it, so the v3 ruleset must parse v3 JSON the v3 way (branch v3.1.1 map.c) |
| Triggers / KrunkScript | v3 triggers (single action) | v7 triggers, KS | v10 additions (actions limit 8, archetypes) | per-version trigger and action tables; KS API gated by version |
| Ranked / seasons / clans | KN's own ranked (v3 branch) | v7 ranked (eloK 32 ...) | v10 seasons (ranked.md) | server-side (kn_brain) per version; client windows per page |
| UI | v3.1.1 page (branch v3.1.1 `ui/`) | v7 page (`ui/`) | v10 (ui.md: much of it Svelte, so it needs a new import plus C ports) | `ui/v3/`, `ui/v7/`, `ui/v10/` page trees, C UI logic split where windows differ |
| Assets | v3 models/textures | v7 sets | v10 asset paths (ui.md) | shared `assets/` with per-version overrides only where a file differs |
| Network | protocol 1.x of the v3 branch | 1.3.1 | new packets (net.md) | the version is a handshake field, one ruleset per server process |

## Id clashes with KrunkNative additions

| Clash | v10 | KN today | Resolution |
|---|---|---|---|
| Weapon ids | 29 Boulder, 30 Compressor (gameplay.md) | 29 Tool Gun, 30 Piss, 31 Physics Gun (decisions.md) | move KN weapons above every ruleset's table (e.g. 64+, `WEAPON_ID_KN_BASE`) and keep a remap for saved loadouts/skins; v10 keeps Krunker's ids on the wire |
| Window ids | 53 customize, 54 title_selection (ui.md) | 54 Object Manager (decisions.md) | renumber the Object Manager to a KN range (e.g. 100+) before the v10 page lands |
| Packet names | v10 `e+ e- es ee ea+ ea-`, `pz`, `plk`, ... (net.md) | KN knet ids are numeric, 250-254 used, 1-byte type exhausted | v10 packets need the "protocol decision" decisions.md asks for (2-byte type or a sub-typed v10 envelope) |

## Recommended architecture

1. `shared/include/ruleset.h`: `typedef enum { RULESET_V3_1_1, RULESET_V7_1_4, RULESET_V10_0_2 } RulesetId;`
   and a `Ruleset` struct of const pointers: `GameConstants`, weapon table + count, class table + count,
   mode table, map catalogue, `MapDialect`, trigger/action tables, KS feature mask, UI page directory.
   `const Ruleset *ruleset_get(RulesetId)`.
2. `Game` gains `const Ruleset *rules`. `game_init` fills `weapons`, `classes`, `maps`, `modes` from it
   (they exist already). `game_constants` becomes a macro/inline reading `game->rules->constants` where a
   `Game` is in scope. As a first step, a `const GameConstants *g_rules_constants` set once per process works,
   because one server process runs one ruleset and the offline sandbox is a separate in-process server
   (client/src/net/offline.c). That keeps the 266 references a mechanical rename.
3. Tables live in `shared/src/rules/v3/`, `rules/v7/` (today's files moved), `rules/v10/`. v3 data comes from
   branch v3.1.1 (`git show origin/v3.1.1:shared/src/...`), not rewritten by hand.
4. Map parsing: `map_parse(json, MapDialect)` with the dialect from the ruleset. A v3 map under the v7/v10
   ruleset keeps today's behaviour (v7 reads v3 JSON); under the v3 ruleset it uses v3 semantics.
5. Server: `server_bin --ruleset v3|v7|v10` (server.cfg `ruleset`), sent in INIT. The server browser and
   matchmaker show the ruleset; the client switches its ruleset to the server's on join. Offline: the menu
   toggle picks the sandbox ruleset.
6. Client UI: a settings toggle (Settings > General, "Game version") saved in config, with a restart of the
   UI page (`html_ui_load_page` with `ui/<ver>/index.html`, plus a fresh C UI init). The C UI logic stays one
   codebase; window handlers check `rules->id` only where the pages differ. The v3 page is the branch's `ui/`.
7. Tests: run proc_input_test movement fixtures per ruleset (v3 fixtures from the v3.1.1 branch, v7 the
   current ones, v10 new recordings).

## Cost and risks

| Item | Estimate / risk |
|---|---|
| GameConstants pointer refactor | mechanical, 43 files |
| `g_weapons` / `g_classes` direct users to `game->` | 45 files, moderate; some client code has no Game in scope (menus, viewer), so it needs `ruleset_current()` |
| v3 tables from the branch | v3 weapons/classes/modes structs differ from the v7 structs (fields added since). Each v3 entry must be refilled in the v7 struct, with defaults that reproduce v3 behaviour. This is the largest correctness risk |
| v3 map/trigger semantics | branch map.c/triggers.c differ from main; porting them as a dialect is work comparable to one map-logic package |
| UI: three pages | the v3 page needs the old C handlers (branch client/src/ui), which diverged. Simplest: v3 page + v3 handler set behind `rules->id`, accepting duplication |
| Protocol | v3-branch clients and servers don't speak 1.3.x. With one binary, all rulesets speak the current protocol with a ruleset field; old binaries stay incompatible (acceptable) |
| Backend (kn_brain) | ratings/seasons/stats per ruleset (a `ruleset` column), otherwise v3 and v10 stats mix |

Recommendation: implement v10 first as the new default ruleset with the ruleset seam (steps 1-3, v7 and v10
only), then add v3.1.1 as a third ruleset once the seam is proven. That avoids doing the v3 refill twice.
