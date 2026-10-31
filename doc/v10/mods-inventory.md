# Mods and chat commands: v7.1.4 inventory against the v10.0.3 path

Scope: every client mod in the v7.1.4 mod tree (`mod/*.ts`, `main/*`) and every command in v7's
command set (the developer console and the chat `/` commands), checked against KrunkNative on the
v10.0.3 path: the native client against a v10 ruleset, and chat commands as they arrive from the
bridge's `CHAT_SEND` path. The official v10.0.3 client has none of these and is not changed.

The v7 reference is `/home/user/jails/kru/src/Krunker_v7.1.4` (read only). Line numbers are into
that tree (v7) or into this repository at `ff4a851b` plus this change (KN).

## Verification level

- **run**: a test in this change, or an existing test that passed here (named).
- **read**: checked by reading the code path; not run in this pass.

## Result, in counts

| Set | Items | Have | Missing | Broken on v10 | Notes |
|---|---|---|---|---|---|
| Chat / server commands (v7 `registerServer` + the server switch, `server/index.ts:2854-3236`) | 56 | 48 (incl. `bhop`, client-side; and `modes`, `maps` after their fix) | 2 remaining (`xp`, `kr`) | 2 fixed (`modes`, `maps` were DEV-only; v7 has no gate) | 6 added: `endvote`, `time`, `endtime`, `serverinfo`, `status`, `classes` |
| Console-only commands (v7 `registerLocal`, `mod/devConsole.ts`) | 31 | 26 | 1 remaining (`ent_bbox`) | 0 | 4 added: `resetbinds`, `reset_binds`, `third`, `tpmesh` |
| Client mods (`mod/mods.ts`, `mod/*.ts`, `main/*`) | 30 | 18 | 1 (`ent_bbox`); 1 partial (F6 editor); 1 moved (F1) | 0 | 9 are browser-page or v7-sandbox workarounds, not applicable |
| Bridge: official `ct` chat (`server/bridge/v10_bridge.c`) | 1 path | 0 (fixed) | 0 | 1 fixed (every chat line from the official client was dropped) | fail-before test |

The 56 chat names and the 30 console names overlap (`ping`, `maps`, `status`, `serverinfo` appear in
both); the console list above counts only the names that exist only in the console.

Remaining gaps are listed at the end with the reason.

## 1. Chat and server commands

`v7` = `server/index.ts` switch (`handleDevConsoleCommand`, line 2854) and the chat `ct` handler
(`server/index.ts:4272`, which handles `/end`, `/endvote`, `/xp`, `/kr`). `v7 gate` is the gate the
v7 code applies (`requireHostOrDev`, `requireCheatAccess`, `requireAuth`, or none). `KN` is
`shared/src/chat_commands.c` (table `COMMANDS` at line 2689). `KN gate` is the `CHAT_PERM_*`
(`shared/include/chat_commands.h:24-28`).

| Command | v7 source | v7 gate | KN source | KN gate | v10 path | Status |
|---|---|---|---|---|---|---|
| god | index.ts:3105-3125 | host / dev / premium | chat_commands.c `god` | CHEAT | chat | have |
| ungod | (none in v7) | | chat_commands.c `ungod` | CHEAT | chat | KN addition |
| noclip | index.ts:3105-3125 | host / dev / premium | chat_commands.c `noclip` | CHEAT | chat | have |
| notarget | index.ts:3126-3145 | dev only | chat_commands.c `notarget` | CHEAT | chat | have; wider than v7 (premium host), see deviations |
| smite | index.ts:3105-3125 | host / dev / premium | chat_commands.c `smite` | CHEAT | chat | have |
| forcehide | index.ts:3105-3125 | host / dev / premium | chat_commands.c `forcehide` | DEV_CHEAT | chat | have; narrower (dev only) |
| unlimitedammo | index.ts:3105-3125 | host / dev / premium | chat_commands.c `unlimitedammo` | DEV_CHEAT | chat | have; narrower (dev only) |
| c / count | index.ts:2936-2942 | none | chat_commands.c `c`, `count` | ANY | chat | have |
| bhop | devConsole.ts:1941 (registration); the client intercepts `/bhop` (devConsole.ts:5478) | client | ui_console.c:1756 `bhop_command`, 1797 `ui_console_chat_command` (chat_send in ui_controls.c:1217) | client | native only | have (client; the chat `/bhop` never reaches the server in v7 or KN) |
| xp | index.ts:3165-3171; ct `/xp` 4272-4290 | host / dev | not in table | | chat | **missing**: needs a brain grant route; not ported |
| kr | index.ts:3165-3171; ct `/kr` 4272-4290 | host / dev | not in table | | chat | **missing**: needs a brain grant route; not ported |
| kill | index.ts:3146-3155 | cheat + target | chat_commands.c `kill` | CHEAT | chat | have |
| end | index.ts:2910-2914; ct `/end` 4272 | host / dev | chat_commands.c `end` | DEV | chat | have |
| endvote | index.ts:2915-2919; ct `/endvote` 4282 | host / dev | chat_commands.c `cmd_endvote` (957) | DEV | chat | **fixed** (was missing) |
| time | index.ts:2920-2927 | host / dev | chat_commands.c `cmd_time` (921) | DEV | chat | **fixed** (was missing) |
| endtime | index.ts:2928-2935 | host / dev | chat_commands.c `cmd_endtime` (941) | DEV | chat | **fixed** (was missing) |
| endless | index.ts:3172-3178 | host / dev | chat_commands.c `endless` | DEV | chat | have |
| borders | index.ts:3179-3187 | host / dev | chat_commands.c `borders` | DEV | chat | have |
| forcecol | index.ts:3188-3192 | host / dev | chat_commands.c `forcecol` | DEV | chat | have |
| server | index.ts:2952-2980 | none | chat_commands.c `server` | ANY | chat | have |
| serverinfo | index.ts:2953 | none | chat_commands.c table alias (2710) | ANY | chat | **fixed** (was missing on the chat path) |
| status | index.ts:2954 | none | chat_commands.c table alias (2711) | ANY | chat | **fixed** (was missing on the chat path) |
| mode | index.ts:2994-3014 | host / dev (switch) | chat_commands.c `mode` | DEV | chat | have |
| modes | index.ts:2991-2993 | none | chat_commands.c `cmd_modes` (1092) | ANY | chat | **fixed**: was DEV-only in KN |
| map | index.ts:3033-3046 | host / dev (switch) | chat_commands.c `map` | DEV | chat | have |
| maps | index.ts:3015-3032 | none | chat_commands.c `cmd_maps` (1178) | ANY | chat | **fixed**: was DEV-only in KN |
| cycle | index.ts:3033-3046 | host / dev | chat_commands.c `cycle` | DEV | chat; and the cycle bind (NET_PACKET_CYCLE) | have |
| game | index.ts:3229-3232 | host / dev | chat_commands.c `game` | DEV | chat | have |
| team | index.ts:3047-3063 | none (spawned) | chat_commands.c `team` | ANY | chat | have |
| teams | index.ts:3064-3077 | none | chat_commands.c `teams` | ANY | chat | have |
| class | index.ts:3088-3104 | none | chat_commands.c `cmd_class` (2591) | DEV, offline sandbox only | chat | have; narrower (KN decision, see deviations) |
| classes | index.ts:3078-3087 | none | chat_commands.c `cmd_classes` (966) | ANY | chat | **fixed** (was missing) |
| players | index.ts:2981-2985 | none | chat_commands.c `players` | ANY | chat | have |
| ais | index.ts:2986-2990 | none | chat_commands.c `ais` | ANY | chat | have |
| ping | index.ts:2943-2951 | none (dev may target) | chat_commands.c `ping` | ANY | chat | have |
| kick | index.ts:3199-3212 | host / dev / canGlobalKick | chat_commands.c `kick` | DEV | chat | have; narrower (no host, no moderators; command-auth.md parity note) |
| ban | index.ts:3199-3212 | host / dev / canGlobalBan | chat_commands.c `ban` | DEV | chat | have; narrower (same) |
| unban | index.ts:3213 (prints only) | host / dev | chat_commands.c `unban` | DEV | chat | have |
| bans | index.ts:3214 | host / dev | chat_commands.c `bans` | DEV | chat | have |
| ip | index.ts:3221-3228 | host / dev | chat_commands.c `ip` | DEV | chat | have; narrower (no host) |
| banip | index.ts:3215 | host / dev | chat_commands.c `banip` | DEV | chat | have |
| unbanip | index.ts:3216 | host / dev | chat_commands.c `unbanip` | DEV | chat | have |
| ipbans | index.ts:3217 | host / dev | chat_commands.c `ipbans` | DEV | chat | have |
| score | index.ts:3156-3164 | cheat | chat_commands.c `score` | DEV_CHEAT | chat | have; narrower (dev only) |
| summon | index.ts:3193-3198 | host / dev | chat_commands.c `summon` | DEV_CHEAT | chat | have; narrower (no host) |
| game (text) | index.ts:3229 | host / dev | chat_commands.c `game` | DEV | chat | have |
| bot / bot_add | handleDevConsoleBotCommand, index.ts:2893 | host / dev | chat_commands.c `bot`, `bot_add` | HOST_DEV | chat | have |
| bot_type | same | host / dev | chat_commands.c `bot_type` | HOST_DEV | chat | have |
| bot_place | same | host / dev | chat_commands.c `bot_place` | HOST_DEV | chat | have |
| bot_kick | same | host / dev | chat_commands.c `bot_kick` | HOST_DEV | chat | have |
| bot_kill | same | host / dev | chat_commands.c `bot_kill` | HOST_DEV | chat | have |
| bot_list | same | host / dev | chat_commands.c `bot_list` | HOST_DEV | chat | have |
| bot_config | same | host / dev | chat_commands.c `bot_config` | HOST_DEV | chat | have |
| bot_mimic | same | host / dev | chat_commands.c `bot_mimic` | HOST_DEV | chat | have |
| bot_stop | same | host / dev | chat_commands.c `bot_stop` | HOST_DEV | chat | have |
| bot_difficulty | same | host / dev | chat_commands.c `bot_difficulty` | HOST_DEV | chat | have |

Counted: the 56 v7 names. The table also has the KN addition `ungod` and a second row for `game`
(the text form), which are not counted. Have 48 (incl. `bhop`, client-side, and `modes` / `maps`
after their permission fix), added 6 (`endvote`, `time`, `endtime`, `serverinfo`, `status`,
`classes`), missing 2 (`xp`, `kr`).

Chat-path check (`/` commands from the official client): the v10 `ct` frame carries the text,
and the bridge now forwards it (section 3). With the 10.0.2 shape (`["ct", channel, text]`), the bridge before this change dropped every chat line
from the official client, so no `/` command reached `chat_commands.c` from there.

## 2. Console-only commands (`registerLocal`)

| Command | v7 source | KN source | v10 path | Status |
|---|---|---|---|---|
| help | devConsole.ts:734 | ui_console.c `help` | native | have |
| clear | devConsole.ts:746 | ui_console.c `clear` (cls too) | native | have |
| echo, print | devConsole.ts:750-754 | ui_console.c `echo`, `print` | native | have |
| bind | devConsole.ts:758 | ui_console.c:1580 | native | have |
| unbind | devConsole.ts:779 | ui_console.c:1581 | native | have |
| resetbinds | devConsole.ts:794 | ui_console.c:1610 (`cmd_resetbinds`, 1397) | native | **fixed** (was missing) |
| reset_binds | devConsole.ts:795 | ui_console.c:1611 | native | **fixed** (was missing) |
| alias | devConsole.ts:1411 | ui_console.c `alias` | native | have |
| exec | devConsole.ts:1433 | ui_console.c `exec` | native | have |
| connect | devConsole.ts:1622 | ui_console.c `connect` | native | have |
| disconnect | devConsole.ts:1657 | ui_console.c `disconnect` | native | have |
| quit | devConsole.ts:1670 | ui_console.c `quit` | native | have |
| reconnect | devConsole.ts:1674 | ui_console.c `reconnect` | native | have |
| offline | devConsole.ts:1754 | ui_console.c `offline` | native | have |
| sandbox | devConsole.ts:1764 | ui_console.c `sandbox` | native | have |
| ahk | devConsole.ts:1774 | ui_console.c `ahk` | native | have |
| version, build, verison | devConsole.ts:1778-1786 | ui_console.c | native | have |
| say | devConsole.ts:1801 | ui_console.c `say` (sends plain chat) | native | have (client-only in v7 too; no `/say` chat command exists in v7) |
| status, serverinfo | devConsole.ts:1807-1811 | ui_console.c | native | have |
| pos, position, rot, rotation | devConsole.ts:1845-1857 | ui_console.c | native | have |
| ping | devConsole.ts:1861 | ui_console.c `ping` | native | have |
| toggle | devConsole.ts:1791 | ui_console.c `toggle` | native | have |
| thirdperson | devConsole.ts:1881 | ui_console.c:1607 | native | have |
| third | devConsole.ts:1887 | ui_console.c:1608 | native | **fixed** (was missing) |
| tpmesh | devConsole.ts:1893 | ui_console.c:1609 | native | **fixed** (was missing) |
| show_navmeshes | devConsole.ts:1128 | ui_console.c `show_navmeshes`, nav_overlay.c | native | have |
| ent_bbox | devConsole.ts:1392 | none | native | **missing** (remaining, see gaps) |
| maps | devConsole.ts:1962 (local) and the server switch | console forwards to the server `maps` | chat | have |

Counted: 31 console-only names. The table also lists `ping`, `maps`, `status` and `serverinfo`,
which section 1 counts. Have 26, fixed 4 (`resetbinds`, `reset_binds`, `third`, `tpmesh`),
remaining 1 (`ent_bbox`).

Console forwarding: `ui_console.c:1736-1747` forwards only names it registers. Unregistered names
are silent, so the four timer and class names above were also needed as `register_server` entries
(ui_console.c:1634-1637) to reach the server at all.

## 3. Bridge: official `ct` chat

`server/bridge/v10_bridge.c` (`handle_client`, the `V10_OUT_CHAT_MESSAGE` branch, 626-641).

- The v10.0.2 client (`game.deob.js`, the `iíîíiìî` function) sends `CHAT_MESSAGE` as
  `["ct", channel, text]`: `channel` is 0 (global) or 1 (team), `text` is the second argument.
- The old branch read `args[0]` (the channel number) as the text. `v10_encode_chat` returns 0 for
  a non-string, so with that shape **every line the official client typed was dropped**: no chat, no `/` command.
- The captured frame in `server/tests/v10_translate_test.c:1254` is `["ct", text]` (one argument).
  Both shapes are now accepted: a leading number is a channel, a bare string is the text.
- Team channel 1 goes out as `NET_PACKET_CHAT_V7` with channel byte 1 (the server's
  `NetChatV7SendPacket`); global goes out as `NET_PACKET_CHAT_SEND` (unchanged).
- Fail-before: `server/tests/v10_bridge_test.c:1040-1052` failed 4 checks on the old code
  (`timed out waiting for native_count(NET_PACKET_CHAT_SEND) == 2`, and the team frame), and
  passes on the fix (155 checks, 0 failed).
- Not verified live: no capture of the 10.0.3 client's chat frame was available. The deob here is
  10.0.2. The fix accepts both shapes, so it holds whichever one 10.0.3 sends.

## 4. Client mods

| Mod | v7 source | KN source | v10 path | Status |
|---|---|---|---|---|
| Bhop: 5 modes, slidehop, `/bhop` | mods.ts:56-86, 462, 1241; devConsole.ts:5447-5523 | ui_bhop.c:121 `ui_bhop_mode`, 171 `ui_bhop_toggle`, 311 `ui_bhop_fill_input`; ui_console.c:1756 | native (input frame) | have (read) |
| Bhop mode bind | mods.ts:458-459, 613-636 | ui_mods.c (`CAPTURE_BHOP`), ui_logic.h:779 `ui_bhop_set_bind` | native | have (read) |
| Bhop toast | notify.ts; mods.ts:462-483 | bhop_toast.c, bhop_toast.h | native | have (read) |
| Ctrl TP: T toggle, ctrl-click teleport | mods.ts:945-960, 1103-1107, 108-160 | ui_mod_keys.c:59 keydown (T), 111 `ui_mod_keys_ctrl_tp_mouse` (NET_PACKET_TELEPORT) | native; the server gates TELEPORT (CHAT_PERM_DEV_CHEAT, command-auth.md) | have (read) |
| Scroll-wheel weapon swap | mods.ts:999-1088 | main.c:1859-1866, 2304 | native | have (read) |
| Dev console | devConsole.ts (whole); mods.ts:7-37 | ui_console.c, console.h | native; server commands go out as chat (ui_console.c:526-533) | have (run: console-settings e2e 15/15) |
| Dev console bind | mods.ts:889-897, 562-587 | ui_mods.c (`MODS_KEY_DEV_BIND`) | native | have (read) |
| Cycle command bind | mods.ts:454-455, 900 | ui_mods.c (`MODS_KEY_CYCLE_BIND`); main.c:1475 `client_send_cycle_packet` (NET_PACKET_CYCLE) | native; server gate DEV | have (read) |
| Force offline sandbox | mods.ts:7-37, 410-420 | ui_mods.c (`MODS_KEY_FORCE_OFFLINE`) | native | have (read) |
| Player list key (window 23) | mods.ts:484-517 | ui_controls.c:198 (`playerList`), 1360 | native | have (read) |
| Show navmeshes | devConsole.ts:1128 | ui_console.c, nav_overlay.c | native | have (read) |
| Entity bounding boxes (`ent_bbox`) | devConsole.ts:1392 and its overlay | none | native | **missing** (remaining) |
| F4 find new game | mods.ts:916-919 | ui_mod_keys.c:64-70 | native | have (read) |
| F5 reload | mods.ts:920-922 | ui_controls.c (reload bind) | native | have (read) |
| F6 editor, staging the current map | mods.ts:923-944 (`saveVal("mapToEdit", ...)`) | ui_mod_keys.c:73-77 opens `editor.html` but does not stage the map; the editor reads `mapToEdit` (editor.c:986) | native | **partial** (remaining) |
| F1 clear log | mods.ts:911-914 | F1 is the Object Manager key (ui_controls.c:250); `clear` in the console | native | **moved**: the clear-log packet is not bound to a key |
| Quick join (region fix) | index.ts (mod) `quickJoinRegion`, index.ts:116 | servers_ui.c:4 (quick join through seek) | native | have (read; the populated-only filter is not in KN's seek path) |
| Settings rows (dev console, offline, binds) | mods.ts:775-846 | ui_mods.c | native | have (read) |
| Notify toast | notify.ts | ui_notify.c | native | have (read) |
| Elapsed timer (top-right HH:MM:SS) | notify.ts | hud_ui.c `hud_gametime_step`, game_time.c (the Game Time tile, Settings > Client > Game > Show Game Time; signed in: the account's time played) | native + web | have (account-level) |
| Mod packs (canLoadMods, zip) | mods.ts (Mods window) | ui_mods.h `loadModPack`; krunkscript.md:1273 | native | have, partial (resource packs where allowed) |
| Third-person client toggle | devConsole.ts:2674 `runClientThirdPersonCommand` | third_person.c, ui_console.c:1509 | native | have |
| Sandbox noclip reset guard | mods.ts:163-288 | none needed | n/a | n/a: a v7 sandbox-server workaround; KN's offline sandbox has no carried-over noclip |
| Sandbox script sanitiser | mods.ts:179-219 | ks/ (KrunkScript, sandboxed) | n/a | n/a |
| Adblock | adblock.js | none | n/a | n/a: browser page |
| Cookies | cookies.ts | none | n/a | n/a: browser page |
| Page hooks (level-requirement unlocks) | hooks.ts | min_rec in ClassConfig (shared.h) | n/a | n/a: KN's own gates |
| Object injection | objects.js | Object Manager (KN addition) | n/a | n/a: superseded |
| Map store | mapStore.ts | native map catalog | n/a | n/a |
| Bot worker (nextbot) | botWorker.ts | bots.c, bot_*.c | n/a | n/a: native bots |
| Connect state | connectState.ts | ui_console.c `connect` | n/a | n/a: native connect |

Counted: 30 rows. Have 18 (mod packs are partial), missing 1 (`ent_bbox`), partial 1 (F6), moved
1 (F1), n/a 9.

## 5. Deviations kept on purpose (not gaps)

- Moderation (`kick`, `ban`, `ip`, `banip`, `summon`) is DEV only; v7 also let the custom host
  and the `canGlobalKick` / `canGlobalBan` moderators. `doc/v7/command-auth.md` records this as a
  parity note: widening it is a policy change.
- `notarget` is wider than v7 (premium custom host can use it). Kept; command-auth.md lists it
  under CHAT_PERM_CHEAT.
- `class` is DEV and offline sandbox only; v7 let anyone use it. KN's own decision (chat_commands.c
  `cmd_class`, the `class works in the offline sandbox only.` refusal).
- `maps` and `modes` were DEV-only in KN and are now ANY (read-only lists, as v7).

## 6. Remaining gaps

| Gap | Why not done here | What it needs |
|---|---|---|
| `xp`, `kr` (chat, dev: add XP / KR to the own account) | ChatCommandHost has no grant callback; the account economy lives in kn_brain | a brain route for a dev grant, then a host callback like `kpd_officer` |
| `ent_bbox` (console overlay of movement boxes) | client renderer work (a debug overlay like nav_overlay.c, with its own headless test) | a line overlay for player and object boxes; its own e2e |
| F6 stages the current map (`mapToEdit`) | client change with a headless test that is not written yet | ui_mod_keys.c F6: write the loaded map's JSON to `mapToEdit` before opening editor.html; test by the editor e2e |

## Changes in this pass

- `server/bridge/v10_bridge.c` (CHAT_MESSAGE channel shape, team channel) and its test
  `server/tests/v10_bridge_test.c` (fail-before).
- `shared/src/chat_commands.c`: `/time`, `/endtime`, `/endvote`, `/classes`, the `/serverinfo` and
  `/status` aliases; `/maps` and `/modes` open to every peer.
- `shared/src/game.c` and `shared/include/shared.h`: `game_set_end_timer` (v7 `setEndTimer`).
- `shared/tests/target_select_test.c`: `test_timer_commands` (the timer, list, alias and
  permission cases).
- `client/src/ui/ui_console.c`: the four server registrations (`time`, `endtime`, `endvote`,
  `classes`), `resetbinds` / `reset_binds`, `third` / `tpmesh`.
