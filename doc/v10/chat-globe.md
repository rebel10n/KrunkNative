# The chat's globe icon (#chatSwitch)

Reference: Krunker v10.0.3 bundle `~/jails/kru/src/Krunker_v10.0.3/game.js` (minified) and its
decoded copy `~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js` (line numbers and character offsets below are into the
decoded copy, `line:offset`; the minified `game.js` offsets are given where they are the only handle).

## What the icon is

- Markup (identical in v10.0.3 and `ui/index.html:10`): `<div id="chatSwitch" class="material-icons"
  data-tab="public" onclick="switchChat(this)">` inside `#chatSwitchHolder` in `#chatInputHolder`, left of
  the chat input.
- The glyph is the Material Icons ligature `public` (a globe). `ui/css/main.css:1353-1373` (same rules as
  v10.0.3 `css/main.css:1351-1370`): `#chatSwitch:after { content: attr(data-tab) }`, and on hover of the
  holder `content: "autorenew"`. So the icon shows the channel's name as the ligature: `public` for
  Global, `groups` for Team.
- It is not a language, region or translation picker. v10.0.3 has no translate, language or region
  control on the chat box, and the bundle has no other handler for it.

## What a click does (v10.0.3)

`window.switchChat` (deob-10.0.3.js line 259568, char 11327898), in full:

1. `playSelect(0.1)` (`select_0` at volume 0.1), on every call.
2. `cur = data-tab == "public" ? 0 : 1`.
3. Outside a team mode (`!iiïìîii.mode.teams`): `cur = 1` and no channel message.
4. `data-tab = cur ? "public" : "groups"`: the icon flips between Global and Team, or stays Global.
5. `cur = ~~!cur`: the channel the chat filters on (0 Global, 1 Team).
6. Rows of `#chatList` show when their `data-tab` equals the channel or is -1 (every channel).
7. In a team mode only: a system line `"Team" | "Global" + " Text & Voice Chat"` (type 2, channel -1).
8. `chatChannel = cur` (the global variable that sends and voice read; in memory only).

It is called from elsewhere too:
- the round's INIT outside a team mode while the channel is Team, deob line 257141 char 11218525
  (`if (chatChannel == 1 && (!mode || !mode.teams)) switchChat(chatSwitch)`); this forces Global and
  plays the sound;
- the round's INIT for a comp (ranked) game not on Team, deob line 257144 char 11218639
  (`if (chatChannel != 1 && Game.isComp) switchChat(chatSwitch)`): a comp lobby starts on Team, with the
  system line. Minified: game.js offset 10320024.

Nothing is stored: no setting, no localStorage. The channel lives in `chatChannel` for the page's
lifetime.

## What the channel affects

- Sending: a chat line is `CHAT_MESSAGE: "ct"` (deob line 111038, char 3943789) with the channel; a voice
  clip is `VOICE_CHAT: "vc"` with `chatChannel` (`["vc", dataURL, chatChannel, micVolume, ...]`).
- Receiving: a line on another channel is hidden (its row's `display`), not dropped; the rows keep
  their `data-tab`.
- In a non-team mode every line is channel -1 (shown on every channel).
- The server's side (the v10.0.3 bundle holds no chat handler: its embedded sandbox server has none for
  "ct", and the official server is closed), from Krunker v7.1.4's server, `server/index.ts:2065`
  `sendPlayerChat(io, room, game, sender, requestedTeamChannel, message)`, called by its "ct" handler
  (`:4306`, `getChatTeam(args)`: 1 is the team channel, anything else Global):
  - `teamChannel = requested === 1 && !!game.mode?.teams`; the line goes out as
    `"ch", channel, name, escapeHtml(msg), 0, null, chatNameColor, accountId`.
  - Global: `game.broadcast("ch", ...)` to the room.
  - Team: `io.send` to each room member whose player has the sender's team (`target.team === sender.team`),
    the sender included; a member without a player (a spectator) gets nothing.
  - A socket without a player sends nothing at all (`if (!player) return;` before the call).
  So the enemy team never receives a team line; the client-side `data-tab` filter only picks what the
  viewer looks at among the lines it did receive.

Nothing else hangs on the channel: no placeholder change on `#chatInput` ("Enter Message" stays), no
tooltip, no prefix or colour on a team line (the row is `name: msg` on either channel), no key binding
(`switchChat(` is called from the onclick and the two INIT lines only), no clan or party channel.

## Our port (before this change)

- Markup and CSS: same as v10 (`ui/index.html:10`, `ui/css/main.css:1353-1373`).
- Click: `ui/index.html` `onclick="switchChat(this)"` -> the inline handler `switchChat` ->
  `client/src/ui/hud_ui.c` `hud_ui_handle` (the `switchChat` branch, `hud_ui.c:4813`) ->
  `hud_switch_chat` (`hud_ui.c:2526`). That function was already the v10 toggle: the same mode test,
  the same `data-tab` flip, the same row filter, the same system line.
- Missing: the `playSelect(0.1)` click sound. Before the change a click started no `select_0` at all
  (client/tests/chat_globe_e2e.py, A and B, fails with clicked 0 / control 0).
- The forced-Global INIT path (`hud_chat_channel_check`, `hud_ui.c:2512`) and the comp start
  (`hud_round_page`, `hud_ui.c:4449`) were silent; v10 plays the sound on both, since both call
  `switchChat`.
- Sending uses the channel: `client/src/net/game_events.c:92-93` (`NetChatV7SendPacket.channel`),
  voice `client/src/voice.c:1641`.

## Why a click can look like it does nothing

- In game the cursor is locked, so a click is a shot, not a page click. Chat typing (Enter) does not free
  the cursor in v10 or in our port, so the icon is reachable through the Escape menu only. The same holds
  in v10 (the browser's pointer lock swallows the click).
- In a non-team mode (Free for All and most maps) a click keeps Global and shows nothing, in v10 as well.
  Only the team modes show a change.

## Change in the first pass (commit 6f169309)

- `hud_switch_chat` and `hud_chat_channel_check` play `select_0` at 0.1 (`audio_play`), as v10's
  `switchChat` does on every call.
- No other behaviour changed: the toggle, the row filter, the system line, the channel sent, and the
  lack of persistence already match v10.0.3.
- Test: `client/tests/chat_globe_e2e.py` (target `chat-globe-test`).

## Second pass: the channel end to end (both server types, the official client)

What was already there and verified against the references above:

- Server (`server/src/net.c` `net_handle_chat`): the team channel only in team modes, a team line to the
  sender's team only (`net_teammates`: both have a player, same team); the sandbox server
  (`client/src/net/offline.c`) runs the same core. v7 peers get `NET_PACKET_CHAT_V7` ("ch" with the
  channel, `shared/include/social_v7.h`), older peers the v3 line.
- Client receive (`client/src/net/game_events.c` `NET_PACKET_CHAT_V7` -> `client/src/ui/hud_social.c`
  `social_on_chat_v7` -> `hud_ui_add_chat_channel`): the row's `data-tab` is the packet's channel (-1
  outside team modes), hidden while the viewer's channel differs, as v10's row builder.
- Voice: `client/src/voice.c` sends `chatChannel`; the server relays a team clip to the team only
  (`net.c` `team_only`), the client plays a clip whose channel is -1 or its own (v10 deob :243845).
- The official client on a Box Office server (`server/bridge/v10_bridge.c:642-657`): `["ct", 1, text]`
  becomes `NET_PACKET_CHAT_V7` channel 1, a bare `["ct", text]` Global; outgoing lines keep their channel
  (`server/bridge/v10_translate.c` `v10_map_chat_v7`, the v3 line as -1).
- Box Office on an official server (`client/src/net/krunker_link.c:695-720`, `:3094-3110`): "ch"'s tab
  reaches the HUD as the row's channel; our line goes out as `["ct", channel, text]`.

What differed from v10.0.3 and changed:

- The forced Global of an INIT outside a team mode (deob :257141) ran only on the next HUD pass of a
  living player (`client_hud_html_tick`), so a round that rotated to Free for All while the player sat on
  the end screen or the menu kept the globe on Team. `hud_round_page` (the INIT) now calls
  `hud_chat_channel_check` first, then the comp check, in v10's order.
- The comp start (deob :257144) tested `mode.teams` before calling `hud_switch_chat`; v10 calls
  `switchChat` whenever `chatChannel != 1 && Game.isComp` and lets switchChat keep Global outside a
  team mode (with its sound). The test is gone.
- Server: a team-channel line from a connection without a player (a spectator, or one still in the menu
  before its first spawn) was sent as a Global line to everyone, the enemy team included. It now stays on
  the team channel and reaches the sender alone (no teammates). The v7.1.4 reference drops every chat line
  of such a socket; Box Office keeps letting spectators talk on Global, so only the team line differs, and
  the sender sees it echoed instead of lost (documented at `net_handle_chat`).

Test (`make chat-globe-test`, `client/tests/chat_globe_e2e.py`): the offline A-C runs, then D / E on a
`server_bin --mode tdm` with two protocol peers on the two teams, a spectator peer and the headless
client: the client's team line reaches its teammate (channel 1) and never the enemy or the spectator,
the enemy's team line never reaches the client's page, the spectator's team line reaches nobody else,
the enemy's Global line reaches everyone (a hidden tab-0 row under Team), the globe's glyph is its
`data-tab`, the placeholder never changes, and the server console's `mode ffa` forces the globe back to
Global at the INIT with the select sound and no channel line.
