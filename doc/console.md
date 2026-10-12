# Developer console and chat commands

KrunkNative ports the Krunker v3.1.1 **offline build's** developer console and its chat
commands one to one. Neither exists in the original game client, which sends every chat
line raw ("ct"). They come from the reconstruction in the jail tree:

- `mod/devConsole.ts`: the console (draggable, resizable "Console" modal, hints, history,
  local commands, server commands sent as `/cmd` chat, singleplayer versions of some).
- `mod/mods.ts`: the toggle key (`krk_dev_console_bind`, default Backquote), the "Enable
  Developer Console (`)" and "Force Offline Mode" switches (General tab, after Mouse Flick
  Fix), and the "Developer Tools" rows in Change Controls (Developer Console, Bhop Mode,
  Cycle Map (/cycle) on F8).
- `server/packetHandlers.ts`: the io_ct command table (`cmds`) that answers `/cmd` lines.

## Where things live

| Part | File |
|---|---|
| Console (DOM, keys, drag/resize, commands, offline handlers) | `client/src/ui/ui_console.c`, `client/include/console.h` |
| Console stylesheet (verbatim) and font | `ui/css/devconsole.css`, `ui/css/fonts/lucon.ttf` (loaded with `html_ui_add_stylesheet`) |
| mods.ts rows, toggle/F8/B keys, KeyboardEvent.code mapping | `client/src/ui/ui_mods.c`, `client/include/ui_mods.h` |
| Server-side `/` commands (server_bin and the offline game) | `shared/src/chat_commands.c`, `shared/include/chat_commands.h` |
| Wiring | `server/src/net.c` (`net_chat_host`, deferred kicks, IP-ban check on accept), `server/src/cli.c` (CLI runs them as the console, `op`/`deop`), `client/src/net/local_game.c` (1-peer host), `client/src/net/game_events.c` (`NET_PACKET_CHEAT`: noc/unl/fh/borders/forcecol) |
| Runtime stylesheets | `html_ui_add_stylesheet()` (`client/include/html_ui.h`; engine.c, web_dom.c, stub.c), test `client/src/html/engine/tests/test_runtime_sheet.c` |

Permissions match v3.1.1 PERMS: off by default, so everyone may run every command.
`server_bin --perms` (or `PERMS=1`) restricts DEV commands to clients from 127.0.0.1 and
clients `op`'d on the server CLI. The cheat commands (god/smite/noclip/kill) use the
same rule, since KrunkNative has no premium accounts. A denial answers `notify` ("DEV
only" / "DEV or premium custom host only"), as v3.1.1 does; the mod's toast shows it.

## Local console commands (devConsole.ts `registerLocal`)

All output strings and colours are devConsole.ts's (log #e0e0e0, info #9ac8ff, warn
#ffb84d, error #ff6a6a, cmd #fff, reply #9affb0, hint #b0b0b0). Unknown commands are
silent (only the `] line` echo), as in the mod.

| Command | v3.1.1 behaviour | Native |
|---|---|---|
| help [cmd] | "N commands:" + sorted list / `name args — desc` / "unknown command: x" | same (73 commands) |
| clear | empties the log | same |
| echo / print text | prints the text | same |
| bind [key] [cmd] | session key binds by `e.key` / `e.code` ("bound k -> cmd", "no binds", "k = (unbound)") | same; fired on keydown while hidden, not while typing |
| unbind key | "unbound k" | same |
| alias [name] [cmd] | session aliases, registered as commands ("alias for: …") | same |
| exec a;b | runs `;`-separated commands | same |
| connect [local\|host] | points the matchmaker at a host and seeks a game | connects the TCP client to `host[:port]` (IPv4/localhost; "local" = 127.0.0.1:default). Prints "Connecting through …" and "Server: tcp://…". On the web: "endpoint updated; live seek is unavailable on this page" |
| disconnect | resets the endpoints and goes back to "/" | leaves the session/offline game, back to the default server, menu open. On the web: `location.href = '/'` |
| quit | `window.close()` | closes the window (web: `window.close()`) |
| reconnect | `location.reload()` | reconnects to the last server or lobby, also after `disconnect` (offline, or after a `disconnect` from the offline game: restarts it on the same map). The Reconnect bind (F5 by default, Settings > Controls > General) runs it natively; on the web that key stays `location.reload()`, and the address bar (`?game=<id>`, `?offline=1&map=<key>`, `?sandbox`) brings the reload back to the same game |
| offline [map] | `playCustomMap()` + clearPingCounter | starts the embedded offline game, ping 0 (`--offline`, web `?offline=1`); a map (catalogue key, name or path under assets/maps) picks where it plays, like `--map` / `?map=` |
| sandbox | – (v7.1.4: `?sandbox` / `?test` load `config.blankMap`) | KrunkNative: the offline game on the sandbox map, the catalogue's `Test` (maps/custom/Test.json, what the editor's Test plays with no map of its own); `--sandbox`, web `?sandbox` |
| ahk | `IO AHK: <ahNum> (base: 29819)` | `IO AHK: 0 (base: 29819)` (the native protocol never rotates it) |
| version / build / verison | `Version: jhiLQ` | same |
| toggle var | sends `/var` | same |
| say text | sends the text as chat | same |
| status / serverinfo | Connection / Endpoint / Lobby / Players lines | same lines. Endpoint is `tcp://addr:port` natively, `location.origin` on the web |
| pos / position | `position: x y z` | same |
| rot / rotation | `rotation: yaw=… pitch=… xDire=…` | same |
| ping | overwritten by the server command | same (it is the server `/ping`) |

## Server commands (`/cmd` in chat, or the console without the slash)

The console sends `/cmd args` through the chat. In singleplayer, devConsole first runs its
own versions (marked **offline** below), which answer in the console only. In
singleplayer, chat lines also go through them, with or without the slash (the mod's
io.send wrapper). The other commands run in the offline game's chat handler. `/bhop` is
always handled on the client.

| Command | Args | Perm | v3.1.1 behaviour (server reply, magenta Server line) | Native status |
|---|---|---|---|---|
| c, count | – | any | "N connected, M in menu" | ✔ online, ✔ offline (via the game) |
| ping | [name\|id] | any (dev for others) | "Your ping: N ms" / "calculating" / "X's ping: …" / "You can only view your own ping." | ✔ |
| server | – | any | "Lobby … \| region … \| socket …" + "Map … \| mode … \| players … \| tick 30 Hz (33.33 ms)" | ✔ online (lobby "native", region "local"); **offline** = status |
| players | – | any | "Players (n): a, b" / "No players." | ✔ |
| ais | – | any | "AI (n): …" / "No AI." | ✔ every listed AI, dead ones too (shared/src/ai.c) |
| team | red\|blue | any | sets the team and suicides; "Switched to red team." / "Already on red." / "Usage: /team red\|blue" / "/team only works in team modes." | ✔ online, ✔ **offline** |
| teams | – | any | "Red (n): …", "Blue (n): …" / "Not a team mode." / "No players." | ✔ online, ✔ **offline** |
| kick | name\|id | dev | close 4002; "Kicked X." / "Cannot kick yourself." / "Target socket not found." | ✔ (ERROR "Kicked" 004002, removed after the poll pass) |
| ban | name\|id | dev | account ban + close 4003 | ✔ "Banned X (account N)." when server_bin has accounts (the host's ban_account callback); guests: "Disconnected X, but no account id was available to persist a ban." |
| unban | accountId\|name\|id | dev | "Unbanned account N." / "Account N was not banned." / "No valid account id to unban." | ✔ (the list is always empty) |
| bans | – | dev | "Banned accounts (n): …" / "Ban list is empty." | ✔ "Ban list is empty." |
| ip | name\|id | dev | "name: addr" / "(no address)" | ✔ |
| banip | name\|id\|ip | dev | IP ban + close 4003; refusals for your own IP | ✔ (connections from the IP are refused on accept) |
| unbanip | ip | dev | "IP unbanned: x" / "IP was not banned: x" | ✔ |
| ipbans | – | dev | "IP bans (n): …" / "IP ban list is empty." | ✔ |
| end | – | dev | round ends in 3 s; "Match timer set to 3 seconds." | ✔ online, ✔ **offline** |
| endless | – | dev | stops the clock ("t" 00:00) / resumes; "Match timer set to endless." / "Match timer resumed." | ✔ (`game_set_endless`). **offline**: "Match timer paused." / "Match timer resumed." |
| borders | [on\|off] | dev | toggles config.disableB, broadcasts "borders" + "Borders ON/OFF" | ✔ (`NET_CHEAT_BORDERS` keeps client prediction in step), **offline** |
| forcecol | [on\|off] | dev | forces map objects collidable; "Force collide ON (N objects)" / "OFF (N objects restored)" | ✔ (`chat_map_force_collide` on the server's and every client's map). Zones/triggers stay walk-through |
| mode | [alias] | dev | "Current mode: alias (name)" / reloads the map in that mode, broadcasts "Mode set to alias" / "Unknown mode 'x'. Try /modes" | ✔ online; **offline** (mod's resolver, "Mode set to Name (id).", `mode default` clears the cycle override) |
| modes | – | dev | aliases joined | ✔ online; **offline** "Modes (n):" list |
| map | [name] | dev | "Current map: X" / cycleToMap (silent) / "Unknown map 'x'. Try /maps" / "Map is still loading…" | ✔ online; **offline** ("Switched map to X (i).") |
| maps | – | dev | "Maps: …" | ✔ (long lists are split over several lines); **offline** "Built-in maps (n):" |
| cycle | – | dev | cycleToMap(pickCycleMap()) | ✔ online; **offline** "Cycled to X (i) [Mode]." |
| game | text | dev | "Game command received: …" | ✔ |
| god | [target] [on\|off] | cheat | godMode + refill; "God mode ON[ on X]" | ✔ (`Player.god_mode`, "h" sent); **offline** "God mode ON" |
| smite | [target] [on\|off] | cheat | 10000 damage per hit | ✔ (`Player.smite`, applied in `player_change_health`); **offline** |
| noclip | [target] [on\|off] | cheat | "noc" to the target | ✔ (`NET_CHEAT_NOCLIP` sets the client's `me->noclip`, chat "Noclip - Enabled"); **offline** |
| kill | [target] | cheat | suicide / kills every match: "Killed N players + M AIs: …" / "Kill target 'x' not found." | ✔ online (players and AIs: name or sid, exact beats partial); **offline** "You killed yourself." / "Killed player X." |
| forcehide | [target] [on\|off] | dev | "fh" to the target | ✔ (`NET_CHEAT_FORCE_HIDE`, chat "Hide - Enabled"); **offline** |
| unlimitedammo | [target] [on\|off] | dev | "unl" + "uamo" | ✔ (`NET_CHEAT_UNLIMITED_AMMO` + loadout packet); **offline** |
| score | [amount] | dev | "+N score (total T)." / "Usage: /score <amount> (default 100)." | ✔ online; **offline** "Usage: score <amount> (default 100)." |
| summon | [name] | dev | spawns a Soul Sanctum AI | ✔ "Summonable: …" / "Unknown summon 'x'. Try: …" / "Summoned Soul Fiend (21)." (templates = the first BOT per name on soul_sanctum.json, game.ts aliases weep / wander / fiend / angered / gate / lord) |
| bhop | [on\|off\|held] | client | "Bhop - Disabled" / "Bhop - Hold Jump/Crouch" in the console + notify | ✔ (`ui_bhop_set_mode` + `ui_notify`) |

## Bots (Krunker v7.1.4 `bot_*`)

v3.1.1 has no bot commands. They are ported from Krunker v7.1.4 (jails/kru/src/Krunker_v7.1.4):
the registrations and help text of `mod/devConsole.ts`, the replies of `server/index.ts`
`handleDevConsoleBotCommand`, the legacy FTUE `BotManager` of `src/data/bots.js` (+ `names.js`)
and the NextBot manager / AI of `server/NextBotManager.ts` and `bot/nextbot/`. They run in
`shared/src/chat_commands.c` over `shared/src/bots.c` (+ `bot_nav.c`, `bot_names.c`), so the
server console, rcon, `/bot_*` chat and the offline dev console share them. Gate: v7
`requireHostOrDev` = `CHAT_PERM_HOST_DEV` (DEV accounts, ops, the server's own machine, the
custom host; the console and rcon always; offline always), with perms on **or** off.

| Command | Args | Reply (v7 server wording) |
|---|---|---|
| bot, bot_add | [count] [tfue\|autoRespawn\|key=value] | "Spawned N nextbot bots." (count capped at 32; `tfue` = dmgMlt 0.01 like the sandbox Spawn Bot key; errors "Expected key=value, got 'x'", "Unknown bot option 'k'", "Invalid boolean for 'k'") |
| bot_place | [tfue\|autoRespawn\|key=value] | at the caller's feet: "Spawned 1 nextbot bot at your position." (v7's server ignores the position; the console: at a spawn) |
| bot_type | [nextbot\|tfue] | "Bot type: nextbot (use bot_type nextbot\|tfue)" / "Bot type set to tfue; removed N nextbot bots." |
| bot_list | – | "Bots (N): nextbot:1:Name#uid, …" / "No bots." |
| bot_kick | [all\|index\|name] | "Removing N bots." / "No bots matched 'x'." / "No bots." (LEAVE to everyone) |
| bot_kill | [all\|index\|name] | "Killed N bots." / "Matched bots are not spawned yet." (players.kill, skipRewards) |
| bot_config | [key=value …] | "Bot config: accuracy=100 attackTarget=true …" / "Bot config updated (k=v); N live bots updated." |
| bot_difficulty, bot_diff | easy\|normal\|hard\|max\|number | "Bot difficulty: 3" / "Bot difficulty set to 1.8 (N live bots updated; applies to future bot_add)." (botDifficultyConfig: accuracy 25/50/85/100, targetDistance 1200/1200/1800/5000) |
| bot_stop | [on\|off] | "Stopped N live bots; applies to future bot_add." / "Resumed …" (BOT_STOP_CONFIG / BOT_RESUME_CONFIG). No argument toggles. |
| bot_start (bot_resume) | | KrunkNative: resumes bots after bot_stop ("Resumed N live bots; …") |
| bot_mimic | [on\|off] | "Mimic-style follow mode set (N live bots updated)." / "Combat follow mode set (…)." (+ devConsole's mimicTarget: the bot copies its target's input frames) |

The key=value options are BOT_CONFIG_TYPES with its aliases (`god`, `hp`, `skill`, `bhop`, …):
accuracy, attackTarget, autoRespawn, back/body, canRespawn, class, clan, cosmeticRarity,
difficulty, dmgMlt, dye, face, featured, followTarget, forceClass, forceCrouch, godMode,
hairColor, hat, healthMlt, joinMessage, leaveMessage, level, lives, melee, mimicTarget, name,
noRecoil, nodNo, nodYes, premium, persistent, primSkin, randomCosmetics, randomJump, rangeMlt,
removeOnDeath, retreat, roam, scoreOnDeath, secondary, skinColor, slideHop, speedMlt,
spreadMlt, targetBots, targetDistance, targetTeam, team, tfue, useSpawnDelay, voiceChat,
waist, watchTarget (head, pet, shoes, wrist have no v3.1.1 slot). Not in v7.1.4 although
sometimes quoted: bot_attack / bot_hide / bot_statemachine (file names of the CS bot code
NextBot mirrors), bot_profile (a backlog idea) and bot_ftue_v (the FTUE map `bot_ftue_v2.json`).

Bots are players: `players.add` with `isBot`, SKINS + SPAWN + an account-style PLAYER_INFO
(account id 2000000 + id like NextBot's createAccount, a level, a clan tag over level 25, a
featured / premium chance), states, kills, leaderboard rows. Names come from names.js (80%
handles, else colour + separator + noun), classes at random (no Rocketeer / Runner), a
secondary, weapon / cosmetic skins from `assets/skins.json` (array index) under a rarity cap,
lower rarities likelier. server.cfg `bots` / `bot_quota` / `bot_difficulty` (KrunkNative
addition; config.botLimit 16) keep or fill the lobby; the info query reports them as `bots`,
never as `players`, and the coordinator only counts connections. Ranked lobbies get none.

AI (`bot_type nextbot`, the default): Idle / Hunt / Attack / Hide with the BotProfile presets
(reaction time, aim focus, dodge, chase timeout, coward / retreat), aim-down-sights and recoil
control by skill, reloads and the secondary on an empty primary. `bot_type tfue` runs bots.js
Bot.update (turnSpd, roam circle, 2 s re-target, fire inside half the range). Both walk over a
nav grid built from the map (`bot_nav.c`, v7 navmesh.ts idea: walkable tops, headroom, walk /
jump / drop links, A*, no edges / jump pads / kill volumes).

Intentional differences: bots survive round and map changes (v7 game.init removes
non-persistent ones); every bot rolls a realistic identity unless level= / clan= / … are
given (v7 bot_add bots are level 0); cosmetics per slot with a chance; a bot keeps its class
(10% switch per respawn); class= implies forceClass; voiceChat, noRecoil, spreadMlt and
scoreOnDeath are parsed but have no native effect; the sandbox Spawn Bot / Clear Bots keys
(numpad + / -) are not bound.

Not chat commands, so not ported here: the `pnh`, `prmA`, `noclip`/`tp` packets,
`clearLog`, and the `tmc` terminal message (map terminals; see the open items).

## Argument completion (KrunkNative extension)

devConsole.ts completes only the command name: Tab fills the longest common prefix, and
the hints list the commands that start with what you typed. Past the first word it
shows a single row with the command's signature and description. KrunkNative keeps both
and adds completion for arguments:

- The hints list the command's signature row followed by the candidates for the argument
  under the caret, filtered by what is typed. Clicking a candidate fills it in.
- Tab completes a single candidate (plus a space). With several, Tab steps through them
  and Shift+Tab steps back, highlighting the current one (`.hintItem.isActive`) and
  scrolling the 12-row list to keep it visible. Typing anything else ends the cycle.
  Command names cycle the same way instead of stopping at the common prefix.
- Candidates per command:

| Command | Arguments completed |
|---|---|
| help, exec | command names (with aliases) |
| alias | 1: existing aliases, 2: command names |
| bind | 1: keys (bound keys, then `e.key` values a-z / 0-9 / F1-F12 / Space / Tab / Shift / Control / Alt, then `e.code` KeyA… / Digit0…), 2: command names |
| unbind | bound keys |
| toggle | the server toggles: god, noclip, smite, forcehide, unlimitedammo, borders, forcecol, endless, bhop |
| god, noclip, smite, forcehide, unlimitedammo | 1: player names in the game, or on / off; 2: on / off |
| borders, forcecol | on / off |
| bhop | on / off / held |
| kill | player names in the game and the active AIs' names |
| summon | the summonable AI names (Angered Soul, Gatekeeper, Lord of Souls, Soul Fiend, Wandering Soul, Weeping Soul) |
| ping, kick, ban, unban, ip, banip | player names in the game |
| map | the map catalogue (shared/include/map_catalog.h): v7's official maps by name, community maps (among them the v3.1.1 maps, `311_kanji`, and its old ones, `152_burg`) by file name |
| mode | mode aliases (offline also `default` / `reset`) |
| team | red / blue |
| connect | local, the last server address |
| score | 100, 250, 500, 1000, -100 (the signature row shows `amount: number?`) |
| bot_kick, bot_kill | all, the bots' names |
| bot_type | nextbot / tfue |
| bot_mimic, bot_stop | on / off |
| bot_difficulty | easy / normal / hard / max |
| bot, bot_add, bot_place, bot_config | tfue, autoRespawn and common `key=` options |

The other commands take free text (echo, print, say, game, unbanip) or no
argument; for those the signature row is the hint. No console command takes a class,
weapon or setting name, so there are no such candidates. On the web, Shift reaches C
through kn-shim (`x = 1` on key events).

`/map` and `/maps` on the server use the same catalogue, as v3.1.1 cmd_map / cmd_maps do:
`Maps: <official names>` then `Custom: <custom map names>`. A map outside the rotation is
added to the game's list when it is picked.

## Verification (2026-10-10)

- Native offline (`client_bin --offline`, Xvfb + xdotool): every local command, every
  offline handler, and the non-offline server commands through the offline game's chat
  handler. Toggle key, Escape, history, Tab, hints, titlebar drag, grip resize, saved
  geometry (`krk_dev_console_geom`) all work. Side by side with the real offline build
  (headless Chromium, CDP): same modal size (758x294 at 514,8), colours, font and text.
- Online (`server_bin --port 21915 --mode tdm` + two clients, both connected with the
  console `connect`): every server command above, including ban of the other client,
  IP bans, map/mode/cycle, borders/forcecol and the noc/unl/fh client lines.
- Web build (emcmake, `?offline=1`): the console opens with `, runs commands, and drags
  through kn-shim's `data-kn-pointer` glue.
- `client/src/html/engine/tests/test_runtime_sheet.c`: 23/23.

## Known differences

- The autocomplete rows are lighter than in v3.1.1. There, main.css's
  `* { color: #353535 }` overrides the colour `.hintItem` would inherit from `.hints`
  (#d8d8d8), so the inactive rows are #353535 on #6f6f6f. Chrome and the native engine
  render this the same way: the computed colour is rgb(53, 53, 53) in both. At the
  user's request, an override at the end of `ui/css/devconsole.css` gives `.hintItem`
  `color: inherit` (the mod's intended #d8d8d8) and makes `.hintMeta` (argument
  signatures, candidate notes) #cccccc instead of #b8b8b8.
- No hotkey fires while a text field has focus (`ui_text_input_focused()`: an input,
  textarea, select or contenteditable element, #chatInput or #terminalInput). This covers
  the mods.ts keys (F1, F4, F6, T, B, F8, the console key binds), game binds (Reconnect,
  F5, included), the voice key, Ctrl TP clicks, gamepad presses and main.c's F11 / N.
  v3.1.1 still let F5 (reload) and F6 (editor) through. The console key (`) still closes the console from
  its own input, as in v3.1.1.
- devConsole mirrors the browser's `console.*` output into the log. KrunkNative has no
  JavaScript console, so only game lines appear.
