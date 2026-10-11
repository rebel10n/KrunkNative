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
same rule, since KrunkNative has no premium accounts. v3.1.1 answers a denial with a
`notify` packet the client ignores; KrunkNative sends the text ("DEV only") as a Server
line.

## Local console commands (devConsole.ts `registerLocal`)

All output strings and colours are devConsole.ts's (log #e0e0e0, info #9ac8ff, warn
#ffb84d, error #ff6a6a, cmd #fff, reply #9affb0, hint #b0b0b0). Unknown commands are
silent (only the `] line` echo), as in the mod.

| Command | v3.1.1 behaviour | Native |
|---|---|---|
| help [cmd] | "N commands:" + sorted list / `name args — desc` / "unknown command: x" | same (60 commands) |
| clear | empties the log | same |
| echo / print text | prints the text | same |
| bind [key] [cmd] | session key binds by `e.key` / `e.code` ("bound k -> cmd", "no binds", "k = (unbound)") | same; fired on keydown while hidden, not while typing |
| unbind key | "unbound k" | same |
| alias [name] [cmd] | session aliases, registered as commands ("alias for: …") | same |
| exec a;b | runs `;`-separated commands | same |
| connect [local\|host] | points the matchmaker at a host and seeks a game | connects the TCP client to `host[:port]` (IPv4/localhost; "local" = 127.0.0.1:default). Prints "Connecting through …" and "Server: tcp://…". On the web: "endpoint updated; live seek is unavailable on this page" |
| disconnect | resets the endpoints and goes back to "/" | leaves the session/offline game, back to the default server, menu open. On the web: `location.href = '/'` |
| quit | `window.close()` | closes the window (web: `window.close()`) |
| reconnect | `location.reload()` | reconnects (offline: reloads the map). On the web: `location.reload()` |
| offline | `playCustomMap()` + clearPingCounter | starts the embedded offline game, ping 0 |
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
| ais | – | any | "AI (n): …" / "No AI." | ✔ "No AI." (no AI natively) |
| team | red\|blue | any | sets the team and suicides; "Switched to red team." / "Already on red." / "Usage: /team red\|blue" / "/team only works in team modes." | ✔ online, ✔ **offline** |
| teams | – | any | "Red (n): …", "Blue (n): …" / "Not a team mode." / "No players." | ✔ online, ✔ **offline** |
| kick | name\|id | dev | close 4002; "Kicked X." / "Cannot kick yourself." / "Target socket not found." | ✔ (ERROR "Kicked" 004002, removed after the poll pass) |
| ban | name\|id | dev | account ban + close 4003 | ✔ guest semantics: "Disconnected X, but no account id was available to persist a ban." (KrunkNative has no accounts) |
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
| kill | [target] | cheat | suicide / kills every match: "Killed N players: …" / "Kill target 'x' not found." | ✔ online (players only); **offline** "You killed yourself." / "Killed player X." |
| forcehide | [target] [on\|off] | dev | "fh" to the target | ✔ (`NET_CHEAT_FORCE_HIDE`, chat "Hide - Enabled"); **offline** |
| unlimitedammo | [target] [on\|off] | dev | "unl" + "uamo" | ✔ (`NET_CHEAT_UNLIMITED_AMMO` + loadout packet); **offline** |
| score | [amount] | dev | "+N score (total T)." / "Usage: /score <amount> (default 100)." | ✔ online; **offline** "Usage: score <amount> (default 100)." |
| summon | [name] | dev | spawns a Soul Sanctum AI | "Summon unavailable on this game instance." (v3.1.1's reply without AI support) |
| bhop | [on\|off\|held] | client | "Bhop - Disabled" / "Bhop - Hold Jump/Crouch" in the console + notify | ✔ (`ui_bhop_set_mode` + `ui_notify`) |

Not chat commands, so not ported here: the `pnh`, `prmA`, `noclip`/`tp` packets,
`clearLog`, and the `tmc` terminal message (map terminals; see the open items).

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

- The native engine has no CSS `min()`. The modal's `min(744px, calc(100vw - 16px))` and
  `min(280px, …)` are therefore computed in C and written inline until the grip or a
  saved geometry sets a size.
- The engine's font fallback has no glyph for the close button's "✕" (Chrome falls back
  to a system font), and the grip's multi-stop `linear-gradient` stripes aren't drawn.
- devConsole mirrors the browser's `console.*` output into the log. KrunkNative has no
  JavaScript console, so only game lines appear.
