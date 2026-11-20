# Gameplay and server parity audit, 2026-11-20 (Krunker v10.0.7)

Scope: modes, weapons and attachments, classes, physics and movement, map objects, prefabs and triggers,
AI and bots, KrunkScript runtime, packets and server rules, spectating, and the ranked and match flow.
UI, menus, social and the backend are audited separately. The reference is
`~/jails/kru/src/Krunker_v10.0.7/deob/game.deob.js`, cited as `deob-10.0.7.js:<line>`.

## Method

1. **Tables, regenerated from 10.0.7.** `tools/kref/v10-fixtures.py` was run against the 10.0.7 bundle, with the
   table lines moved to their 10.0.7 positions, into scratch, then diffed against `shared/tests/data/v10/` (the
   10.0.3 extraction that `weapons_v10_test` checks the C tables against). The tables and their 10.0.7 lines:
   weaponConfig :99659, classConfig :102638, effectConfig :164539, upgradeWeapons :177550, attachments
   :180663, streaks :180987, scope overlays :202747, and the config object :19503.
   Two of these could not be found by line alone, because Babel and grep count lines differently after a
   long base64 line. Matching the array's length as well found them (`LEN`, a scratch copy of `dump2.cjs`).
2. **Config keys.** Every key of the frozen config object, scalar or not, was compared between 10.0.3 and 10.0.7.
3. **The 10.0.7 changelog's gameplay items** (doc/v10/target.md "10.0.7") were checked against KN's code, citation
   by citation.
4. **The earlier audits' open rows.** doc/v10/gameplay-1003-audit.md, rules-1003.md, entities-1003.md,
   s2c-coverage.md and kn-additions.md were checked for rows still marked open.

## Results

| Area | 10.0.7 vs KN | Status |
|---|---|---|
| weaponConfig (31), classConfig (16), effectConfig (10), attachments (2), streaks (7), scope overlays (7) | byte-identical to the 10.0.3 fixtures | match |
| upgradeWeapons (29) | identical. The Bomb's and the Build Tool's `rot` came out unresolved only because `resolve_pi` reads the old line | match |
| config scalars (354 keys) | one new key, `loadInTime` 2000 (:16546, key :19812). `buildVersion` changed (doc/v10/target.md). Twelve platform flags are no longer scalars (marketEnabled, steamEnabled, xsolla*, ...), none of them gameplay | **gap, fixed** (1) |
| config keys, every type (472) | only `loadInTime` is new since 10.0.3. The rest of the "new" list was a range artifact: each key also exists in 10.0.3 | match |
| rotationMaps (:169614) | KN's `g_rotation_maps` (shared/src/map.c:40) equals 10.0.7. `seek_filter_test` still expected 10.0.3's list (… 40 … 45) | **stale test, fixed** (2) |
| 10.0.7 gameplay changelog items | all cited in code: inputBank (player.c, game.c); reason 8 (krunker_link.c); isGunGamePromotion (player.c, mode_actions.c); entityClock.reset (entities.c); visDis 120 (ks_hostobj.c); the AI part-name key and getPartPosition without preciseHits (ai_parts.c); the AI glow (ai_render.c); the ranked forfeit after POST_MATCH (forfeit.c); the anonymous lobby payload and players.add (comp_lobby.c, game_state.c); Eterno's `mScl` (player_mesh.c, prefab.c) | match |
| packet tables (c2s 138 / s2c 264) | unchanged (`make krunker-build`); s2c-coverage.md has no open rows | match |
| earlier audits' open rows | none in gameplay. The deliberate differences are listed in kn-additions.md and rules-1003.md | match |
| refused spawn (FAILED_TO_ENDER "fe", :98699, handler :214976-214981) | KN's server answered a canSpawn refusal with a chat line only, so the client stayed "entering" (`spawn_requested`) and a second click on Play did nothing. That also affected the comp lobby's "Please join a Team to Spawn" | **gap, fixed** (3) |

### 1. The load-in hold (loadInTime)

10.0.4 added `loadInTime` = 2000 ms (:16546). `finishInit` sets `loadInTimer = isServer && !isSandbox ? loadInTime : 0`
(:193804). While it runs, `canSpawn` answers "Loading the map..." (:191039-191040), and `update` runs no lockout, wait
timers, conditions or game clock (:195342-195345, the `iíÏîîíí` gate :195433). This is the server half of 10.0.4's
"less stutter when a match starts". KN had ported only the client half, the loading card's 2 s hold
(client/src/ui/v10_loading.h).

KN now has `GAME_LOAD_IN_TIME` 2.0 (shared/include/shared.h) and `Game.load_in_timer`, set in `game_start_round`
(shared/src/game.c) when the game has a server and is not the offline sandbox. `game_tick_round` holds the round while
it runs. `game_spawn_block` is v10's canSpawn: the load-in hold, then the comp lobby's checks. Test: `match_test`
`test_load_in`.

### 2. rotationMaps

10.0.4 dropped map 40 from the public rotation and added 46, Candy Heist (:169614). The rotation itself was already
current. Only `client/tests/seek_filter_test.c` still asserted 10.0.3's list.

### 3. FAILED_TO_ENDER from KN's own server

A refused ENT now sends `NET_EXT_KN_OFFICIAL_ENTER_FAILED`, the EXT form of "fe", carrying canSpawn's message
(server/src/net.c `net_send_enter_failed`). The client's existing handler (client/src/net/game_events.c
`events_enter_failed`) shows the menu and the red server line, and clears the enter, so Play works again. A peer
older than EXT still gets the message as a chat line.

The test tools click again the way a player would:
- `server/tests/kn_proto.py` resends its last ENT 0.3 s after the refusal. While that ENT is pending,
  `read(until=SPAWN)` skips the dead SPAWN that comes with the HELLO.
- The input script's menu-mode `play` (`input_script_enter_failed`) also clicks again after a refusal.
- `host_tp_e2e`, `sprays_e2e` and `idle_kick_e2e` wait for an active SPAWN.

## What remains

- Nothing open in this scope. The KrunkScript API, the modes and the prefab / trigger lists were triaged version by
  version as each build landed (doc/v10/target.md 10.0.4-10.0.7, krunkscript-1003.md, entities-1003.md), and this
  audit found no row left open there.
- These server e2e failures predate this audit. Each one fails the same way on a clean build of `origin/main`
  (dee6d6ea) without these changes:
  - `sprays_e2e`, parts 2 and 3: 8 checks, the decal is never logged.
  - `nuke_e2e`.
  - `v10_match_e2e`: 4 checks.
  - `hitmarker_death_e2e`.
  - `idle_kick_e2e`'s kick timing on a loaded machine.

  With these changes, the tests the hold first broke pass again:
  - by retrying ENT in `kn_proto` and the input script: `end_respawn`, `host_tp`, `object_manager`, `bots`,
    `command_auth`, `pickup_chat` and `ai`;
  - `idle_kick`'s spawning;
  - `join_spawn`, `kill_pools_server`, `game_config`, `ks`, `kpd_demo` and `solo_ended` were not affected.
