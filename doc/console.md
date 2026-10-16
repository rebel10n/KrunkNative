# Developer console and chat commands

Krunker Box Office ports the Krunker v3.1.1 **offline build's** developer console and its chat
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

Permissions (KrunkNative, deliberate difference from v3.1.1 PERMS, see
[v7/command-auth.md](v7/command-auth.md)): DEV commands (/kick, /ban, /ip, /map, ...) are for
DEV peers only: brain-verified DEV accounts, `ops`, clients `op`'d on the server CLI and clients on
the server's own machine. The cheat commands (god/smite/noclip/kill/tp ...) also admit the custom
game's host. Only `perms` off **and** `gameserver.cheats "all"` (`--open-cheats`; the offline
sandbox) gives everyone every command, as v3.1.1's perms-off server did. The gate runs on the
server for the connection's own identity before any target or selector resolves. A denial answers
`notify` ("DEV only" / "DEV or premium custom host only" / "Host or developer required"), as
v3.1.1 does, and the server logs `perms denied: <cmd> from <id> (<name>, account <id>)`.

## Local console commands (devConsole.ts `registerLocal`)

All output strings and colours are devConsole.ts's (log #e0e0e0, info #9ac8ff, warn
#ffb84d, error #ff6a6a, cmd #fff, reply #9affb0, hint #b0b0b0). Unknown commands are
silent (only the `] line` echo), as in the mod.

| Command | v3.1.1 behaviour | Native |
|---|---|---|
| help [cmd] | "N commands:" + sorted list / `name args — desc` / "unknown command: x" | same (91 commands, with `give` / `drop`, `cls`, `thirdperson` / `firstperson`, `reload`, `console_reset` and `logout`) |
| clear | empties the log | same; Krunker Box Office alias `cls` |
| echo / print text | prints the text | same |
| bind [key] [cmd] | session key binds by `e.key` / `e.code` ("bound k -> cmd", "no binds", "k = (unbound)") | same; fired on keydown while hidden, not while typing. KrunkNative: `mwheelup` / `mwheeldown` bind the mouse wheel (in game; the bound direction runs its command and does not swap weapons, the other direction swaps as before). `+name` presses a control (`jump`, `shoot`, `crouch`, ...: the v7 bind names, the key or button bound to it) and `-name` lets go: a key bind's `+name` holds until the key is released, a wheel bind's until the frame ends. Typed `+name` / `-name` run the same way (no release by itself) |
| unbind key | "unbound k" | same |
| alias [name] [cmd] | session aliases, registered as commands ("alias for: …") | same |
| exec a;b | runs `;`-separated commands | same |
| connect [local\|host] | points the matchmaker at a host and seeks a game | connects the TCP client to `host[:port]` (IPv4/localhost; "local" = 127.0.0.1:default). Prints "Connecting through …" and "Server: tcp://…". On the web: "endpoint updated; live seek is unavailable on this page". A game link or lobby id (`connect CH2:kz862`, the long `CHITOWN2:kz862` too) joins that game through the matchmaker ("Joining game …") |
| disconnect | resets the endpoints and goes back to "/" | leaves the session/offline game, back to the default server, menu open. On the web: `location.href = '/'` |
| quit | `window.close()` | closes the window (web: `window.close()`) |
| reconnect | `location.reload()` | reconnects to the last server or lobby, also after `disconnect` (offline, or after a `disconnect` from the offline game: restarts it on the same map). The Reconnect bind (unbound by default, Settings > Controls > General) runs it natively; on the web that key is `location.reload()`. F5's full reload (`reload`, below) ends with it |
| reload | `location.reload()` (the mod's F5) | Krunker Box Office: the full reload in place, see [F5: full reload](#f5-full-reload) (F5 itself relaunches the client). On the web: `location.reload()`, and the address bar (`?game=<id>`, `?offline=1&map=<key>`, `?sandbox`) brings the reload back to the same game |
| logout | – | Krunker Box Office: the menu's Logout (`logoutAcc`, account_ui.c `account_ui_logout`): the saved session token dropped and revoked in kn_brain, the social socket signed out, the guest header (v7 and v10 pages), the Account window shown, the lobby rejoined as a guest (servers_ui.c, as after any account change in the menu); "logged out". A guest gets "not logged in" and nothing else. Client-local: nothing reaches the game server or its command auth |
| console_reset | – | Krunker Box Office: the console back at the stylesheet's place and size (top right, 744 x 280 at most) and the saved geometry (`krk_dev_console_geom`) deleted |
| offline [map] | `playCustomMap()` + clearPingCounter | starts the embedded offline game, ping 0 (`--offline`, web `?offline=1`); a map (catalogue key, name or path under assets/maps) picks where it plays, like `--map` / `?map=` |
| sandbox | – (v7.1.4: `?sandbox` / `?test` load `config.blankMap`) | Krunker Box Office: the offline game on the sandbox map, the catalogue's `Test` (maps/custom/Test.json, what the editor's Test plays with no map of its own); `--sandbox`, web `?sandbox` |
| ahk | `IO AHK: <ahNum> (base: 29819)` | `IO AHK: 0 (base: 29819)` (the native protocol never rotates it) |
| version / build / verison | `Version: jhiLQ` | same |
| toggle var | sends `/var` | same |
| say text | sends the text as chat | same |
| status / serverinfo | Connection / Endpoint / Lobby / Players lines | same lines. Endpoint is `tcp://addr:port` natively, `location.origin` on the web |
| pos / position | `position: x y z` | same |
| rot / rotation | `rotation: yaw=… pitch=… xDire=…` | same |
| ping | overwritten by the server command | same (it is the server `/ping`) |
| show_navmeshes [off] | v7.1.4: the nav mesh visualizer (below) | same (v7.1.4 command; v3.1.1 has none) |
| tool_snap [units\|off] | – | Krunker Box Office: the Tool Gun's / Physics Gun's grid step (0.1 to 100 units, or off), the Object Manager's Snap field; no argument prints it. Client-local (stored as `kn_omSnapStep`), so no target selector |
| thirdperson / firstperson | – (Source engine commands) | Krunker Box Office: the over-the-shoulder camera (doc/v7/decisions.md "Third person"). `thirdperson` asks the server (`/thirdperson on`); it switches when the grant arrives ("Third Person - Enabled") or prints the refusal (public / ranked servers, custom games whose host has `knTPCam` off). `firstperson` goes back at once. Also the Controls bind "Third Person" (unbound by default) |

## Server commands (`/cmd` in chat, or the console without the slash)

The console sends `/cmd args` through the chat. In singleplayer, devConsole first runs its
own versions (marked **offline** below), which answer in the console only. In
singleplayer, chat lines also go through them, with or without the slash (the mod's
io.send wrapper). The other commands run in the offline game's chat handler. `/bhop` and
`/clear` are always handled on the client.

| Command | Args | Perm | v3.1.1 behaviour (server reply, magenta Server line) | Native status |
|---|---|---|---|---|
| c, count | – | any | "N connected, M in menu" | ✔ online, ✔ offline (via the game) |
| ping | [name\|id] | any (dev for others) | "Your ping: N ms" / "calculating" / "X's ping: …" / "You can only view your own ping." | ✔ |
| server | – | any | "Lobby … \| region … \| socket …" + "Map … \| mode … \| players … \| tick 30 Hz (33.33 ms)" | ✔ online (lobby "native", region "local"); **offline** = status |
| players | – | any | "Players (n): a, b" / "No players." | ✔ |
| ais | – | any | "AI (n): …" / "No AI." | ✔ every listed AI, dead ones too (shared/src/ai.c) |
| team | red\|blue | any | sets the team and suicides; "Switched to red team." / "Already on red." / "Usage: /team red\|blue" / "/team only works in team modes." | ✔ online, ✔ **offline** |
| teams | – | any | "Red (n): …", "Blue (n): …" / "Not a team mode." / "No players." | ✔ online, ✔ **offline** |
| kick | name\|id\|selector | dev | close 4002; "Kicked X." / "Cannot kick yourself." / "Target socket not found." | ✔ (ERROR "Kicked" 004002, removed after the poll pass) |
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
| god | [target] [on\|off] | cheat | godMode + refill; "God mode ON[ on X]" (selectors: "God mode ON: N players") | ✔ (`Player.god_mode`, "h" sent); **offline** "God mode ON" |
| ungod | [target] | cheat | Krunker Box Office: `god [target] off` ("God mode OFF[ on X]", "God mode OFF: N players") | ✔ |
| smite | [target] [on\|off] | cheat | 10000 damage per hit | ✔ (`Player.smite`, applied in `player_change_health`); **offline** |
| notarget | [target] [on\|off] | cheat | KrunkNative (Source's notarget): bots and AIs never target the player, and drop a current target; "Notarget ON[ on X]" | ✔ (`Player.notarget`, bots.c `bot_is_enemy`, ai.c `ai_find_target`); **offline** |
| noclip | [target] [on\|off] | cheat | "noc" to the target | ✔ (`NET_CHEAT_NOCLIP` sets the client's `me->noclip`, chat "Noclip - Enabled"); **offline** |
| kill | [target] | cheat | suicide / kills every match: "Killed N players + M AIs: …" / "Kill target 'x' not found." | ✔ online (players and AIs: name or sid, exact beats partial; a selector kills the spawned matching players; the server console / rcon kill targets too, with no killer); **offline** "You killed yourself." / "Killed player X." |
| forcehide | [target] [on\|off] | dev | "fh" to the target | ✔ (`NET_CHEAT_FORCE_HIDE`, chat "Hide - Enabled"); **offline** |
| unlimitedammo | [target] [on\|off] | dev | "unl" + "uamo" | ✔ (`NET_CHEAT_UNLIMITED_AMMO` + loadout packet); **offline** |
| score | [amount] | dev | "+N score (total T)." / "Usage: /score <amount> (default 100)." / from the menu or while dead (KrunkNative): "+N score at your next spawn (+T waiting)." (the kept points, summed, added once at the spawn: `PlayerKeep.score`, `player_keep_apply`) | ✔ online; **offline** "Usage: score <amount> (default 100)." |
| summon | [name] | dev | spawns a Soul Sanctum AI | ✔ "Summonable: …" / "Unknown summon 'x'. Try: …" / "Summoned Soul Fiend (21)." (templates = the first BOT per name on soul_sanctum.json, game.ts aliases weep / wander / fiend / angered / gate / lord) |
| bhop | [on\|off\|held] | client | "Bhop - Disabled" / "Bhop - Hold Jump/Crouch" in the console + notify | ✔ (`ui_bhop_set_mode` + `ui_notify`) |
| clear | – | client | Krunker Box Office: empties your own chat log (#chatList); local only, nothing is sent and other players' chat is untouched | ✔ (`hud_ui_clear_chat`, chat only: in the console `clear` / `/clear` is the console's own) |
| thirdperson | [on\|off] | own rule | – (Krunker Box Office) | the third-person camera's permission (`chat_third_person_allowed`): the offline sandbox, DEV peers, the custom game's host and everyone with the host's `knTPCam`, open-cheat servers; else a notify ("Third person is off on this server", "No third person in ranked", "The host has third person off"), "Third Person - Not allowed here (…)" and `NET_CHEAT_THIRD_PERSON` 0. Granted: "Third Person - Enabled" / "Disabled" and the cheat flag 1 / 0 |
| class | <name\|id> [secondary] | dev | – (Krunker Box Office) | switches your class now, in place (`player_switch_class`: the loadout, full health, the class's speed and wall jumps; position, view and inputs kept), and optionally the secondary weapon; a SPAWN tells every client. Offline sandbox only (`ChatCommandHost.class_switched`; elsewhere "class works in the offline sandbox only."). Refusals say why and change nothing: no such class / weapon, ambiguous names, the one class gate `game_class_allowed` ("Nettspend is not playable on official krunker.io servers."), the flavor, the game's class list, a mode's fixed class, a non-secondary weapon, a class with its own secondary. Client side a dev-console command (hidden from help and completion, not run or sent with the dev console off, also from binds and `exec`); class and secondary names complete, names with spaces quoted (`class "Spray N Pray"`) |

## Target selectors (Krunker Box Office)

v3.1.1 and v7.1.4 commands take one player (resolveOnlinePlayerByQuery: the normalised name or
id, exact, else a unique prefix). Krunker Box Office keeps that and adds selectors, in one resolver
(`shared/src/target_select.c`, `shared/include/target_select.h`) behind every player-targeting
command: the chat / console commands (`shared/src/chat_commands.c`, so chat, the dev console,
the offline game, the server console and rcon) and the server CLI's own kick, ban, op, deop and
send (`server/src/cli.c`).

| Selector | Matches |
|---|---|
| `*`, `@all`, `@a` | every player, humans and bots |
| `@me`, `@s`, `!` | yourself (the server console and rcon have no player: "'@me' needs your own player (none here).") |
| `@others` | everyone but you |
| `@bots`, `@humans` | bots / humans only |
| `@team`, `@enemies` | your team (you included) / everyone else; in FFA `@team` is you alone |
| `@alive`, `@dead` | spawned / not spawned |
| `Joel*`, `*bot`, `s?m` | a case-insensitive glob over names (`*` any run, `?` one character), as typed or normalised (`bluebot*` finds "Blue Bot") |

- Commands: god, ungod, smite, noclip, notarget, forcehide, unlimitedammo, kill, kick, ban,
  bring, give (chat / console); kick, ban, op, deop, send (server CLI). Commands that need one
  player (tp, ip, banip, unban, ping) take a selector that matches exactly one ("'*' matches 3
  players: name one."). bot_kick / bot_kill keep their own `all|index|name` matching.
- Replies count the players: "God mode ON: 5 players", "Gave Crossbow: 6 players", "Kicked: 3
  players", "Killed 2 players: A, B.". A selector that leaves one player answers like its name
  ("God mode ON on Sam"); one matching nobody says so ("No players match 'zed*'.", "No spawned
  players match '@dead'.").
- The cheats and give / bring act on the spawned matches; kick / ban on every match but you.
  On/off cheats with no on|off over several players turn everyone ON unless all already are
  (then OFF), so `god *` never leaves a mixed lobby.
- Permissions stay per command: the gate (`allowed()`, give's "others: devs and the host") runs
  before the target is resolved, so a selector never widens who may run a command. A guest's
  `kick *` gets "DEV only" like `kick Name`.
- `ungod [target]` (Krunker Box Office) is `god [target] off`, with god's gate.
- The server CLI matches connections (menu clients and spectators too) by id or exact name, as
  before, and its `kick <selector>` also removes matching bots.
- `give <selector> <weapon|item>` reads a leading selector (`give * sniper`); `give <weapon|item>
  <player|selector>` still works (`give sniper @bots`).

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
| bot_list | – | "Bots (N): nextbot:1:Name#uid bhop:clean, …" / "No bots." (bhop: the bot's hop skill none / learning / clean, off without slideHop) |
| bot_nav | – | Krunker Box Office: "Bot nav: stuck N, replans N (forced N), goals N, unreachable N, falls N; grid N nodes." + "Routes (id:state:waypoint/count): 1:Hunt:3/12 bhop:clean, …" (bhop as in bot_list) (`id:dead` while a bot respawns) (`shared/src/bot_path.c` counters since the manager was created; `server/tests/bot_nav_e2e.py`) |
| bot_kick | [all\|index\|name] | "Removing N bots." / "No bots matched 'x'." / "No bots." (LEAVE to everyone) |
| bot_kill | [all\|index\|name] | "Killed N bots." / "Matched bots are not spawned yet." (players.kill, skipRewards) |
| bot_config | [key=value …] | "Bot config: accuracy=100 attackTarget=true …" / "Bot config updated (k=v); N live bots updated." A mimicTarget key is refused (bot_mimic owns it, one controller at a time). |
| bot_difficulty, bot_diff | easy\|normal\|hard\|max\|number | "Bot difficulty: 3" / "Bot difficulty set to 1.8 (N live bots updated; applies to future bot_add)." (botDifficultyConfig: accuracy 25/50/85/100, targetDistance 1200/1200/1800/5000) |
| bot_stop | [on\|off] | "Stopped N live bots; applies to future bot_add." / "Resumed …" (BOT_STOP_CONFIG / BOT_RESUME_CONFIG). No argument toggles. |
| bot_start (bot_resume) | | Krunker Box Office: resumes bots after bot_stop ("Resumed N live bots; …") |
| bot_mimic | [on\|off] | "Mimic-style follow mode set (N live bots updated); NAME controls them." / "…; NAME took control from NAME2." / "Combat follow mode set (…)." One controller at a time (GameBots.mimic_uid): `on` by a dev takes control, a later dev's `on` takes it from the holder, and every mimicTarget bot copies only the holder's input frames (nobody holding control: the bots stand still). `off` releases control and only the holder (or the console) may turn it off ("NAME controls the bots' mimic: only they can turn it off…"). bot_stop and the holder leaving release control too. The console cannot take control (no player). |

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
lower rarities likelier. server.cfg `bots` / `bot_quota` / `bot_difficulty` (Krunker Box Office
addition; config.botLimit 16) keep or fill the lobby; the info query reports them as `bots`,
never as `players`, and the coordinator only counts connections. Ranked lobbies get none.

Fill mode (`bot_quota N`, on in prod: role kn_gameserver `kn_gameserver_bot_quota`, default
max_players; `kn_gameserver_bots_enabled: false` / `kn_gameserver_instance_bot_quota` turn it off):
- An empty lobby stays empty. With no human in the game the quota bots stay `bot_quota_grace`
  seconds (default 10, so a quick rejoin keeps the match), then all leave at once.
- With humans, bots fill the game to N players (never past max_players): one human = N - 1
  bots. A joining human makes a bot leave at once (`game_bots_human_joined`, not on the quota
  tick), from the human's team in team modes, so teams stay balanced and the join never waits.
  Missing bots (the first human, a leave, `autofill`) join in the next tick, one on every spawn
  nobody stands on (at most 8 a tick); the rest follow as those walk off their spawns (a 16-player
  Sandstorm lobby is full about 0.5 s after the first human spawns), and when every spawn stays
  taken for 1 s one joins onto a taken spawn. A lowered quota takes its extra bots at once.
  A new round holds the fill for 1 s so the round's humans spawn first. New bots go to the
  smaller team.
- Humans are the non-bot players in the game. Spectators and clients still in the menu have no
  player and do not count (v7 `checkPlayerLimit` also counts `players.list` without `isBot`).
- The connection limit (`net_add_client`) counts connections only, so a lobby of bots is never
  "full" to a human. The lobby tuple's player count (brain matchmaker, server browser, quick
  join) and the info query / finder `players` are humans; `bots` rides along in the lobby meta
  and the finder listing for information only.
- Round ends and map / mode changes keep the fill (the bots rejoin the next round). Ranked
  matches and hosted custom games get no quota bots (they leave when one starts).
- `autofill [on|off|<players>]` (Krunker Box Office, DEV peers and the custom host:
  `CHAT_PERM_HOST_DEV`) turns the fill on and off at run time, for the server's lifetime (no file is
  written; a restart reads server.cfg again). Off: every quota bot leaves at once and none come
  (`bot_add` bots stay; a `bot_kick` with the fill off refills nothing); on: the fill runs again with
  the server's quota, else the lobby size; `autofill 6` fills to 6 players. A command that turns it on
  also fills a hosted custom game, never ranked ("No autofill in ranked."). No argument:
  "Autofill ON (fill to 4 players, 3 bots now)". The developer console lists it (`help autofill`) and
  sends it like the other bot commands. A refusal toasts "Only the host or a developer can turn
  autofill on or off." (the table entry's `denied` text). On official krunker.io games the console
  says "Autofill is not available on official krunker.io games." and sends nothing: they have no
  bot fill. Who may run it:
  - offline: the local player, always (the sandbox connects over 127.0.0.1, a DEV peer);
  - a hosted custom game: its host (`premium_host`: the connection's account is the lobby's
    `hostAccountId`);
  - a guest (no account, not an op) on a public or self-hosted server: refused. An owner who runs
    a server on another machine can use rcon or the server console instead, or set `bot_quota 0`
    in server.cfg.
  bot_kick alone does not stop the fill (it refills at once while the fill is on), which is why
  `autofill off` exists.
  The info query's tags carry `autofill` while it runs, the lobby meta `autofill`, `/server` and the
  server console's `status` a bots line. Tests: `server/tests/bot_fill_e2e.py` 2b,
  `server/tests/command_auth_e2e.py` (a guest's `/autofill` is refused and logged),
  `client/tests/console_autofill_e2e.py` (the console's `help autofill`, `autofill off` / `on` reach the
  sandbox). The fill's pace:
  `server/tests/bot_fill_time_e2e.py` (each bot's join time from the human's spawn, under 2 s for
  bot_quota and `autofill`) and bot_test's quota pace test.
- Bots earn nothing: stats, challenges, medals, KR and BP go to accounts only.

Bot chat (`shared/src/bot_chat.c`, Krunker Box Office addition; user, 2026-10-14: "some bots
should be able to say gg, we don't need more than one or two bots saying it and it should be at
different intervals"): bots post chat lines like humans do (`NET_PACKET_CHAT`, kind player, the
bot's uid and name). **Off by default** (`gameserver.bot_chat` false): with it off the bots send no
chat at all and make no AI request, and the chat code does not run. Turn it on with server.cfg
`bot_chat true`, config `gameserver.bot_chat`, env `KN_GAMESERVER_BOT_CHAT=true` or
`--gameserver.bot_chat=true`.
- Match end: with a 70% chance (never more than 3 chatty match ends in a row, never more than 2
  silent ones) one or two bots speak, each on its own delay 0.8-7 s after the end and at least
  0.7 s apart. The first says a plain "gg" variant (always a template); the second (or the lone
  speaker half the time) comments the result in character.
- Kills / deaths: the killer (8%) or the victim (6%) bot comments 0.6-2.5 s later, at most one
  such line per 10 s across all bots, one per 40 s per bot, 6 lines per bot per round; nothing
  on the end screen.
- Personality (chill / tryhard / salty / hype / goofy / quiet), stable per bot name + uid, picks
  the template pool and the AI's character. Every line, template or AI, passes
  `bot_chat_filter` (first line only, quotes and a "Name:" prefix dropped, 70 characters, links,
  markup and model refusals refused, profanity and slurs refused in plain, leetspeak, spaced-out
  and stretched spelling); a refused AI line falls back to the template.
- The AI path (`GameHooks.bot_banter`, `server/src/bot_banter.c`): a server linked to kn_brain
  with a signed-in player in the game asks the brain (`bot-banter` frame, doc/backend.md 5.2;
  `services/brain/ai.c`: one non-streamed completion with a tight prompt, `ai.banter.*` limits,
  the answer cut to its first line) and posts the answer when it arrives, or the template 6 s
  after the line's delay. Guests-only lobbies, standalone servers and the offline game get
  templates only. server.cfg / config `bot_chat` (default false) silences or enables the bots;
  `bot_chat_ai false` keeps them on templates. Tests: `bot_chat_test`, `server/tests/bots_chat_e2e.py` (a stubbed
  AI gateway: `make bots-chat-e2e`).

AI (`bot_type nextbot`, the default): Idle / Hunt / Attack / Hide with the BotProfile presets
(reaction time, aim focus, dodge, coward / retreat), aim-down-sights and recoil control by skill,
reloads and the secondary on an empty primary. Perception (`bot_sense.c`, v7 vision.ts): known
threats with last known positions, line of sight to the gut / chest / head / feet (game.canSee),
the recognize gate, heard and hit stimuli, forgotten 5-10 s (by tier) after the last stimulus;
a target that breaks line of sight is chased to its predicted position and then searched for
around it until the memory expires (v7's 2-4 s chase timeout is gone). Krunker Box Office adds
peripheral vision (a close target outside the 90-120 degree FOV), hearing (gunfire within
900-2600 units, running footsteps within 120-260, by tier), a tracking error that decays while
locked on (reset after a 300 ms gap, larger on a fast-crossing target, scaled by accuracy),
target leading (projectile flight time; the slew lag of hitscan), bursts that rest and pause
while the recoil climb settles, and hide spots that break the threat's line of sight.
`bot_type tfue` runs bots.js
Bot.update (turnSpd, roam circle, 2 s re-target, fire inside half the range). Both walk over a
nav grid built from the map (`bot_nav.c`, v7 navmesh.ts idea: walkable tops at the body's
width, headroom, walk / jump / drop links checked for parapets and floors in the way, A* with a
closest-reachable fallback, no edges / jump pads / kill volumes) through the follower of
`bot_path.c`: waypoints consumed when reached or passed, corner cutting onto the next one when
the straight walk is clear, every move code swept against the map before it is pressed (a
ground-following body sweep: walls and overhangs refused, low obstacles hopped only onto the
route's own level, no stepping off an edge the route does not drop from, players side-stepped),
a replan that routes around the spot after 0.8 s without progress and a stuck recovery (back
off, hop, new goal) after a second one. The grid and the sweeps model Krunker's movement: the
climb height (3), low obstacles standing on a floor (a hop lands on top), ramps that lift a
walker onto their surface from the side as well as up the slope, ladders that are walked past
(and let go of, away from the ladder, when one holds the body above the route), seams between
floor tiles that the body bridges, cylinders as the circles the body collides with, border
objects as the 500-unit walls (borderH) they are; hops at a jump link wait until the run-up heads
at the ledge. A start or goal with no node in reach (a goal inside a solid, like oasis_v3's hill
centre) snaps to the nearest node; a point overhead (a Kill Confirmed tag or a flag dropped
mid-jump) is jumped for, then given up. Whatever chose the move (walking, Attack's dodges, mimic), `bot_path_guard` drops a
move or a hop that would carry the body off a void edge (nothing above deathY to land on) and
brakes a drift toward one; a one-way drop into a pit with no way back out costs A* extra.
`bot_nav` prints the counters ("falls": bots that died under deathY); `bot_test` runs 8 roaming
bots on Kanji, Sandstorm, Industry, v7's SkyTemple and FRVR Strike's SkyTemple (and fighting on
both islands) and fails on stuck recoveries, bots pressing into walls or bots falling off;
`bot_stuck_test` checks that every spawn of every rotation map is routable and that six bots on a
fixed list of maps and modes never stand stuck over 5 s or run into one spot again and again
(`KN_STUCK_ALL=1`: every rotation map in every mode the bots play).
`shared/include/bot_path.h` is the read-only view of a bot's route (`bot_path_current`) for the
spectate overlay and bot movement.

Intentional differences: bots survive round and map changes (v7 game.init removes
non-persistent ones); every bot rolls a realistic identity unless level= / clan= / … are
given (v7 bot_add bots are level 0); cosmetics per slot with a chance; a bot keeps its class
(10% switch per respawn); class= implies forceClass; voiceChat, noRecoil, spreadMlt and
scoreOnDeath are parsed but have no native effect; the sandbox Spawn Bot / Clear Bots keys
(numpad + / -) are not bound.

Slide hopping (`slideHop` / `bhop`, `shared/src/bot_move.c`) is on by default for nextbot bots
(v7 tfue default off): instead of v10's "jump on every grounded frame once the slide has 50 ms
left" a bot hops on the first frame that can jump after a landing, lets the crouch go while
rising and holds it while falling (every landing slides), and simulates the jump first,
sweeping its box along the arc against the map: no hop into a ceiling or wall, off a ledge,
onto a jump pad / kill volume or past a corner of its route, and no AI jump (ledge, dodge,
randomJump) under a ceiling it would hit; in the air it re-checks the rest of the flight every
frame and lets the key (or the air strafe) go when its aim turns the flight toward a solid.
Hopping is a per-bot skill, rolled from the bot's personality seed (name + uid) and correlated
with difficulty: `none` (never hops), `learning` (waits on the ground and loses speed,
sometimes forgets the crouch so the landing does not slide, hesitates, skips landings) or
`clean` (hops on the next frame after every clear landing). Mix: easy 75% none / 25% learning;
normal 30% none / 35% learning / 35% clean; hard 15% learning / 85% clean; max all clean. The
safety checks apply to every skill.

## Build load fixture (Krunker Box Office `build_fill`)

`build_fill [count] [kind]` (`CHAT_PERM_HOST_DEV`, like `tp`) places `count` player builds
(default 100, at most 2000; the 1200 `config.buildMax` cap still applies) of `BUILD_OBJS` kind
0-3 (default 0, the Build Block) through `game_build_object`, the Build Tool's own server path:
every client gets the "bo" packets. They stack in layers of 10 x 10 starting 30 units along +x
of your feet and cost no score. Reply: "Built N Build Blocks (M builds)." / "Spawn first.". It is
a load fixture for placement and frame-time tests (`client/tests/build_place_e2e.py`).

## Teleports (Krunker Box Office `tp` / `bring`)

v3.1.1 has only the `tp` packet (the mod's Ctrl TP: a camera-ray hit). v7.1.4's admin menu
moves players (`server/index.ts` handleAdminAction 4 "go to", 5 "bring", 9 "bring all", through
`movePlayerTo` = `players.changePosition(p, x, y, z, doRecon, stopMomentum, dir, true)`) but has
no console command for it. Krunker Box Office adds two, in `shared/src/chat_commands.c`, so chat, the
dev console, the server console, rcon and the offline game share them. Gate: `CHAT_PERM_HOST_DEV`
like `bot_*` (DEV accounts, ops, the server's own machine, the custom host; the console and rcon
always; offline always); everyone else gets the notify "Host or developer required".

| Command | Args | Reply |
|---|---|---|
| tp | x y z | "Teleported to 10.00 2.00 -35.50." (your feet go there, the coordinates `pos` prints) |
| tp | player | "Teleported to Name." (to that player's feet) / "Player 'x' not found." / "Player 'x' is ambiguous." / "Name is not spawned." / "You are already there." |
| bring | player\|all\|selector | "Brought Name to you." / "Brought N players to you." / "Nobody to bring." |
| bring | player\|all x y z | "Brought Name to 0.00 10.00 0.00." / "Brought N players to …" |

Players match like `/kick`: the name or id with everything but letters and digits removed,
case-insensitive, exact first, else a unique prefix (`bring wand` brings "Wandering Ghost").
Bots are players. `all` (or `*`) is every spawned player except you; from the server console
or rcon there is no "you", so `bring` needs coordinates there and `tp` is refused. Dead
players are not moved. Without arguments both print their usage; `tp` before you spawn
answers "Spawn first.".

The move is the server-authoritative `changePosition` with doRecon and stopMomentum: velocity
zeroed, the moved player's own client snaps to it (POSITION + a full RECON), everyone else gets
CHANGE_POSITION ("chgp") and snaps the player instead of interpolating. Unlike v7 (which passes
the destination's yaw in radians as the quarter-turn `dir` and spins the player), the facing is
kept.

Tests: `server/tests/sandbox_test.c` (test_tp_bring: positions, packets, a guest refused,
unknown names, a bot by id) and `server/tests/host_tp_e2e.py` (server_bin + two protocol
clients + rcon).

## Object Manager (Krunker Box Office `give` / `drop`)

Neither v3.1.1 nor v7.1.4 has them: they put the Object Manager's weapons and items
(`shared/include/object_manager.h`, F1) on the console, in `shared/src/chat_commands.c`, so chat,
the dev console, the server console, rcon and the offline game share them. The perm column is
`any`: the Object Manager's own rules decide (`object_manager_caps`, the same as its menu), not
PERMS. Extra weapons wherever its Weapons tab works (devs anywhere; everyone in the offline
sandbox, and in custom games while the host's `knOmWeps` allows; never public, ranked or comp),
items and the Tool Gun where it spawns (devs, the custom game's host, the offline sandbox).
Giving to another player: devs and the host only. The server console and rcon count as a dev.

| Command | Args | Reply |
|---|---|---|
| give | weapon | "Crossbow added to your loadout." An extra loadout slot, the Weapons tab's left click (`OM_ACTION_ADD_WEAPON`): the connection keeps it across respawns, the client equips it when its LOADOUT_EXTRA lands |
| give | item | "KR Tag dropped at your feet." The Entities tab's spawnables and scrap pickups (`object_manager_entity_giveable`; not the machines or an airdrop), at the receiver's feet |
| give | weapon\|item player | "Gave Famas to Name." |
| give | selector weapon\|item, weapon\|item selector | "Gave Crossbow: 6 players" (every spawned match; others: devs and the host) |
| give | – | "Usage: /give <weapon\|item> [player\|selector] (…)" |
| spawn | object\|entity\|npc | "Spawned Crate." An Objects / Entities / NPCs entry by name (matched like `give` over the three tabs: `spawn crate`, `spawn "jump pad"`, `spawn ghost`, `spawn bot`) at your crosshair, under the Object Manager's rules (devs, the host, the sandbox) |
| drop | – | "Dropped the Crossbow." / "Tool Gun put away." / "You can't drop your melee weapon." / "You can't drop your class weapons: only picked-up and Object Manager weapons drop." |

Names match like the Object Manager search, normalised to letters and digits: exact first,
else a unique prefix, else a unique substring (`give alien` is the Alien Blaster, `give krtag`
the KR Tag, `give 13` the Crossbow by id, `give piss` the KrunkNative Piss (id 30, doc/v7/decisions.md "Piss"); a weapon wins over an item of the same name). Quote
names with spaces for a target (`give "desert eagle" Bob`); without a target the whole rest is
the name. Refusals: "Unknown weapon or item 'x'.", "'x' matches N weapons and items: be more
specific.", "Only developers and the host can give to other players." (plus the notify "Host or
developer required"), "Not allowed: extra weapons are off in this game." / "Not allowed: only
developers and the host can spawn here." (plus the same notify), and the Object Manager's limits.

`drop` follows v7.1.4's dropWeapon (players.js, the drop key Z, `dropKey`, "drpw"): a weapon
picked up from a map pickup goes back into that pickup at your feet; never a melee, never a
class weapon (v7 drops only what `lastPicked` knows), never the last weapon, not in noPickups
modes. An Object Manager extra slot, which v7 doesn't have, is tossed as a new weapon pickup
`OM_DROP_TOSS` (8) units in front of you, short of a wall and on the floor below
(`OM_ACTION_DROP_HELD`, the `OM_CAP_WEAPONS` rule), and leaves the loadout; anyone may pick it
up. The Tool Gun is put away instead. Z does the same: the server routes a "drpw" on an extra
slot to `OM_ACTION_DROP_HELD` and answers with SPAWN_RESULT (the client's toast), so no extra
bind is needed.

`give "physics gun"` and `give "tool gun"` hand out the Object Manager's building guns (the
spawn right; doc/v7/decisions.md "Physics Gun", "Building tools"). They have no console commands
of their own (`give physgun` / `give toolgun` work too): the Physics Gun's rotate key is "Physics Gun
Rotate (held)" (R) and its snap key "Physics Gun Snap (held with Crouch)" (E: Shift+E snaps the carry to
45 degrees and the grid) under Sandbox in Change Controls, the grid step is the Tools pane's Snap field,
PageDown / PageUp ("Tool Gun Snap Down / Up") or `tool_snap`, and the Physics Gun's Release is a
row of the Tools pane. The Tool Gun's Properties tool (the pane's 7th tile) opens an object's map
fields in the window; the Properties key (I, "Tool Gun Properties" under Sandbox) does the same for
the selection, else for what the crosshair is on. Devs and the host change the map's own objects
with both guns too (doc/v7/decisions.md "Map edits", "Building tools").

Not spawned in (the menu before the first spawn, dead, spectating), the commands still act on the
connection's player state (`PlayerKeep`, shared.h), which the server keeps on the connection and
applies at every spawn: `god`, `ungod`, `noclip`, `notarget`, `smite`, `forcehide` and
`unlimitedammo` without a target answer "God mode ON" and the next
spawn has them (the cheats also outlive a map change's new player); `tp <x> <y> <z>` / `tp <player>`
answers "You will spawn at …" and the next spawn is there; `give <weapon>` joins the loadout of the
next spawn, `give <item>` and `spawn` (devs) drop at your view: where your last life ended, else the
map's menu camera (straight down from it when the look meets nothing). The Object Manager window's
requests from the menu work the same for devs (`object_manager_request_view`: the camera's look);
for everyone else the menu answers "Spawn first" as before.

Devs (the server's verified DEV accounts, ops, its own machine; not the host) have no Object
Manager caps or pace: spawns past the per-player and per-game counts, requests and edits as fast as
they come, objects moved and carried into and through players (doc/v7/decisions.md "Physics Gun").

Tests: `server/tests/object_manager_test.c` (test_give_drop, test_public_refusals, test_edits, test_map_edits,
test_fields_table, test_field_edits, test_dev_privileges, test_menu_state, test_physgun_player, test_interp) and
`server/tests/object_manager_e2e.py` parts 4 to 8.

Not chat commands, so not ported here: the `pnh`, `prmA`, `noclip`/`tp` packets,
`clearLog`, and the `tmc` terminal message (map terminals; see the open items).

## Nav mesh visualizer (Krunker v7.1.4 `show_navmeshes`)

v7.1.4's dev console (`mod/devConsole.ts` "Nav mesh visualizer") draws the generated
Source-style nav mesh of the current map in 3D: `show_navmeshes` builds and shows it,
`show_navmeshes off` (or `0` / `clear`) removes it. Any other argument (`on`) shows it.

| Reply | When |
|---|---|
| `nav: generated source-style overlay N areas, M nodes, K obstacle links` (info) | shown |
| `nav: overlay cleared` (info) | `off` |
| `nav: renderer/scene not available (are you in a game?)` (error) | no map loaded |
| `nav: no current map data captured yet` (error) | the map has no JSON |
| `nav: no walkable ground under this map's spawns (nothing to draw)` (warn) | Krunker Box Office, after the first line, when the mesh is empty (v7 prints only "0 areas, 0 nodes") |

The mesh is v7's `buildGeneratedNavMesh` (`shared/navgen.ts` + the `extractBoxes` /
`extractLadders` it uses from `shared/navmesh.ts`), ported to C in `shared/src/navgen.c`
(`shared/include/navgen.h`, with `findGeneratedNavPath` and `resolveGeneratedNavArea`): nodes
flood-filled on a 12-unit grid from the spawns, merged into rectangular areas, area
connections and links (walk / jump / drop / crouch / ladder). It reads the map JSON only, so
the client builds it from the current map like v7's mod does in the browser, offline and
online alike; no server data is involved. Counts and paths equal v7's on every official map
and the v3.1.1 maps (`navgen_test`).

The drawing (`client/src/nav_overlay.c`) is v7's, line for line: area fills black at 0.16
opacity, area borders red (inset 0.2), crouch areas a blue diagonal, obstacle areas a second
red outline, and from each area centre toward each connected area: two-way connections cyan,
one-way blue arrows, jump links green arrows, drop links red arrows, crouch links blue arrows.
All of it ignores depth (three.js `depthTest: false`) and is fogged with the map's fog, after
the world scene and before the first-person weapon. It goes through `scene_render()` with
`LineMaterial` (`client/src/materials/line.c`, `assets/shaders/line.*`): GL_LINES and
triangles, no `glPolygonMode`, the same on GL 4.5, ANGLE and WebGL 2.

Map switches: the overlay belongs to one map. The console's offline map switches (`map`,
`mode`, `cycle`, `offline`, `sandbox`: v7's `offlineInitAt` / `runOfflineSandbox`) rebuild it
on the new map; any other map change (rotation, a server's `map`) clears it, as v7's
`ensureNavOverlayOwnerCurrent` does.

v7's overlay state also holds a bot path group (`navOverlay.pathGroup`), but nothing ever
fills it: `show_navmeshes` draws no bot paths in v7.1.4, so neither does Krunker Box Office. The
entity bounding-box overlay is a separate command (`ent_bbox`), not part of this one.

Krunker Box Office differences:
- Two v7.1.4 navgen bugs are fixed (`shared/src/navgen.c`). Each gave an empty mesh: v7's
  overlay reported "0 areas, 0 nodes, 0 obstacle links" and drew nothing.
  - navmesh.ts drops objects whose `l` / `col` key merely exists. The engine treats them as
    collidable when the value is false (map.js `noCol: v.l || v.col`). Older editor exports put
    `"col": false` on every object, so v7's mesh of Bhop_VIP, ClashRoyale, Slide_Neon_Stage,
    SniperRoyale2, Verb_Arena and zombie_high was empty. KN reads `l` / `col` by truthiness and
    `nai` by presence, like the engine.
  - walkableSeeds snaps each spawn to the 12-unit grid. On Industry (v7 and 3.1.1) every snapped
    spawn sits within the body radius of a wall, so nothing seeds the flood fill. When no spawn
    seeds, KN tries the grid points around each spawn (two rings, nearest first), which gives
    Industry 1061 nodes / 564 areas.
  Every map where v7 builds a mesh keeps v7's exact mesh (45 of the 47 official and v3.1.1 maps;
  the other two are the Industries). ESP_Parkour is still empty: its spawn stands only on
  non-colliding (`l: 1`) objects, so the console adds the warning above.
- The build runs on a worker thread natively and the result is cached per map, so the frame
  never waits for it and `show_navmeshes` after `off` is instant. v7 builds inside the command
  (in the browser: 60 to 400 ms on most official maps, 7.5 s on Lagoon, 9 s on Little Town,
  22 s on Shipwrecked). The C build (-O2) takes 2 to 30 ms on most maps and 0.1 to 0.2 s on
  Lagoon (50 000 nodes). The web build has no threads and builds inside the command.
- In v7's offline sandbox, its bots get the generated mesh only while the overlay is on
  (`OfflineNextBotManager.mapData`). Krunker Box Office's bots don't depend on the overlay: they
  always path over `bot_nav.c`'s grid (see Bots). They do not use the generated mesh: v7's
  links climb up to 21 units (jumpHeight 18 + climbHeight 3) while a standing jump peaks near 14
  (jump 0.072, gravity 0.00015 + 0.000032 while rising),
  and they ignore jump pads and kill volumes. In 120 s runs with 8 bots on nine maps, pathing
  on the generated mesh passed bot_test and bots_e2e but got bots stuck much more often:
  stuck recoveries went from 2 to 62 on Shipyard and from 17 to 47 on Little Town.

Reference capture: `tools/kref/navmesh-ref.js <map.json> "<x,y,z,pitch,yaw>" out.png` runs
v7's own `show_navmeshes` in headless Chromium against a v7 server (recipe in the script); the
native counterpart is `client_bin --offline <map>` with `KRUNKNATIVE_CAMERA` at the same pose,
`KRUNKNATIVE_FOV=90`, and an input script running `con show_navmeshes` and `shot`. The
expected numbers in `shared/tests/navgen_test.c` come from v7's `navgen.ts` run under node (an
esbuild bundle of `buildGeneratedNavMesh` + `findGeneratedNavPath`) on the same JSON.

## Argument completion (Krunker Box Office extension)

devConsole.ts completes only the command name: Tab fills the longest common prefix, and
the hints list the commands that start with what you typed. Past the first word it
shows a single row with the command's signature and description. Krunker Box Office keeps both
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
| bind | 1: keys (bound keys, `mwheelup` / `mwheeldown`, then `e.key` values a-z / 0-9 / F1-F12 / Space / Tab / Shift / Control / Alt, then `e.code` KeyA… / Digit0…), 2: command names |
| unbind | bound keys |
| toggle | the server toggles: god, noclip, smite, forcehide, unlimitedammo, borders, forcecol, endless, bhop |
| god, ungod, noclip, smite, notarget, forcehide, unlimitedammo | 1: player names in the game (and the selectors once `*`, `@` or `!` is typed), or on / off; 2: on / off |
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
| tp | 1: player names in the game (bots included) |
| bring | 1: all and the player names in the game (bots included) |
| give | 1: the Object Manager's weapons (the Tool Gun too) and giveable items, 2: player names |
| bot_type | nextbot / tfue |
| bot_mimic, bot_stop | on / off |
| show_navmeshes | on / off |
| bot_difficulty | easy / normal / hard / max |
| bot, bot_add, bot_place, bot_config | tfue, autoRespawn and common `key=` options |

The other commands take free text (echo, print, say, game, unbanip) or no
argument; for those the signature row is the hint. No console command but `give` takes a
class, weapon or setting name. On the web, Shift reaches C
through kn-shim (`x = 1` on key events).

### Console geometry (Krunker Box Office)

The console's place and size are saved as `krk_dev_console_geom` (devConsole.ts: left, top and
the outer `offsetWidth` / `offsetHeight`) after a titlebar drag or a grip resize. v3.1.1 clamps
the box into the window on load and while dragging, but measured it while the overlay was
`display:none` (size 0), so a saved left of 5000 from a bigger monitor only came back to the
window's right edge, entirely outside, and nothing clamped it again after a resize, F11, or an
F4 / F5 restart into a smaller window. It also wrote the saved outer width back as the CSS
(content-box) width, so the box grew by its 14 px padding at every save. Now
(`client/include/win_geom.h`, shared with the editor's Quick GUI and side panels):

- on load, on every viewport change (resize, F11, another monitor) and when the console is
  shown, the whole box is fitted into the window: moved inside, and shrunk to the window when
  it is larger (never below the stylesheet's 280 x 160 minimum);
- a titlebar drag cannot take any of it outside; the grip stops at the window's right and
  bottom edges;
- the saved size is the outer box and is written back as the outer box;
- `console_reset` puts it back at the default place and size.

The automatic fits do not overwrite the saved geometry; only a drag or resize does. The editor's
Quick GUI (which QuickSettings let a drag take anywhere) keeps at least its 120 x 24 px titlebar
strip in the window, and the editor's side panels never grow past what the page leaves beside the
other panel.

### Never hidden while it is on (Krunker Box Office)

devConsole.ts appends the overlay to `<body>` with `z-index: 999999` and only on the game page;
the console is shut with `display:none` and starts closed on every load. In the native client it
"randomly disappeared" because:

- the v10 page has layers above 999999 (the view stack 2147483646, `#popupHolder`, popup windows,
  the loader, tooltips) and appends some of them to `<body>` after the console (the login view
  stack), and a `custom.css` can raise anything: the overlay now has the highest z-index (in
  devconsole.css and inline, so it holds without the sheet) and `ui_console.c` keeps it
  `<body>`'s last element, so it also wins every tie;
- natively the client draws GL over the page after compositing it (the item view's 3D showcase,
  the confetti): the overlay is the page's top layer (`html_dom_set_top_layer`), which
  `html_ui_render_top()` draws after that GL (`html_ui_render_page()` draws the rest);
- the editor, hub and viewer pages had no console, and coming back to the game page built a
  second overlay on the kept document: the console is built (or found again) in every document,
  with its whole log (the last 1000 lines are kept in C), and keys and mouse reach it there too
  (the toggle key only: no game or mod keys on those pages);
- the close button hid it but left its input focused, so the toggle key typed a ` into the
  hidden field instead of opening it: hiding always lets go of the focus;
- a pasted or recalled line holding a toggle character closed it: only a single typed toggle
  character does (`InputEvent.data`, as devConsole.ts);
- its open state did not survive a reload: it is kept as `kn_dev_console_open` (F5, the F4 / F5
  restart, page switches, the web build's reload and the next start).

There is no indicator while it is closed: neither devConsole.ts (v7.1.4) nor Krunker v10 shows
one (v10.0.2 has no developer console at all), so the toggle key (Settings > Controls >
Developer Tools, default `) is the way back, and it now always works. The checks are
`client/tests/console_visible_e2e.py` (input script probe `console @label`) and, on Krunker Strike's page
(ui/strike/) and the default page, `client/tests/console_strike_e2e.py` (`make console-strike-test`): it opens,
takes a typed command and reads its output back.

## F5: full reload

F5 (the Reload Game bind, `cont_gameReloadKey`, Settings > Controls > General) relaunches the
client natively (mm-token, doc/v10/matchmaker-token.md "Keys"): the process re-executes itself as
F4 used to (the same command line, the window kept as it is) and the new process comes back to
the same session (`KRUNKNATIVE_RELOAD_RESUME` below; a lobby is the new process's startup seek).
A reload always goes back into the same game: a KrunkNative lobby by its game id (also when F5 is
pressed while it is still loading), an official krunker.io game by its krunker.io game id, never a
seek by the Quickplay settings (Auto Seek on Launch), an `excludeGame` seek, or what the command line
alone would start (`--offline`, `--krunker`). Only when that game is gone (`InvalidGameId`) or full
(`GameFull`) does the client seek another one, and a notice says so ("The game you were in has
ended: joining another one" / "... is full: ...").
F4 is Find New Game now (Krunker's client: a new seek per Settings > General > Quickplay, no
restart). The console's `reload` is the full reload in place described here.

The console's `reload` reloads the client the way the browser's F5 reloads Krunker: settings,
binds and every asset come from disk again, then the player is back in the same game. It works in game and in
the menu (also with the console open or a text field focused, as a function key), and keeps the
stored settings, the account and the session. The Reconnect bind, which had F5 before, is unbound
by default now (`reconnect` in the console).

Natively, in place, in this order (`client/src/game_reload.c`):

1. Settings: the Client tab (`knClient`, custom.css, the resource swapper folder) read again and
   each changed live row applied (window resolution, raw input, Exit button, Discord, audio output,
   HS sound); the game settings (`knSettings`) read again and pushed, a setting the file no longer
   has back to its default, the page's controls synced, the UI scale applied; the binds
   (`cont_*`), the console mods (toggle key, F8 cycle, force offline) and Ctrl TP read again.
2. Assets: every cached texture read again into the same GL texture and every OBJ model into the
   same geometry, so everything already drawn follows (through the resource swapper / a map's
   resource pack like any load); GLTF models and sounds are read again at their next use (playing
   sounds stop); the menu's window templates (`ui/windows`) are read again.
3. Page: an open menu window is generated again (Settings shows the new values) and the HUD is
   rewritten.
4. Back into the game: the console's `reconnect`. Offline the local game restarts on the same map
   and mode (the map is built again with the new textures and models); online the client rejoins
   the same server (a matchmade lobby is sought again by its game id for a fresh ticket, an official
   krunker.io game by its id; gone or full, another one with a notice, as for F5). In the menu with no game,
   it stays there; when nothing can be rejoined it returns to the menu. A notice says what happened.

What a running process cannot load again makes `reload` restart the client instead, like F5 (the same
command line, the window kept as it is), and the new process comes back to the same session
(`KRUNKNATIVE_RELOAD_RESUME`: the offline map and mode, the server address, the lobby id once the
matchmaker answers, or the official krunker.io game id and region):

- a file of the menu page tree changed (`ui/`, or the `KRUNKNATIVE_UI_DIR` tree; not
  `ui/windows`, which reloads in place) since the client started or last reloaded;
- a shader (`assets/shaders`) or a map file (`assets/maps`) changed;
- a startup-only Client setting (the red "*" rows: audio backend, process priority, resource
  swapper on / off, start offline, account mode, account service / server finder URL) changed.

The log says which (`[reload] restart: ...`). If the restart cannot exec, everything else still
reloads in place. On the web F5 is the browser's reload: the page, its stylesheets, images and
every asset load again, and the address bar brings the reload back to the same game.

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
  the mods.ts keys (F1, F4, F6, T, B, F8, the console key binds), game binds (Reconnect
  included), the voice key, Ctrl TP clicks, gamepad presses and main.c's F11 / N. Reload Game
  (F5) still works there when it is on a function key, as in v3.1.1; v3.1.1 also let F6 (editor) through. The console key (`) still closes the console from
  its own input, as in v3.1.1.
- devConsole mirrors the browser's `console.*` output into the log. Krunker Box Office has no
  JavaScript console, so only game lines appear.
