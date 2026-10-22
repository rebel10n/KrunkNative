# The chat's globe icon (#chatSwitch)

Reference: Krunker v10.0.3 bundle `/home/user/jails/kru/src/Krunker_v10.0.3/game.js` (minified) and its
decoded copy `~/.cache/kn-v103/deob-10.0.3.js` (line numbers and character offsets below are into the
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

## Change in this pass

- `hud_switch_chat` and `hud_chat_channel_check` play `select_0` at 0.1 (`audio_play`), as v10's
  `switchChat` does on every call.
- No other behaviour changed: the toggle, the row filter, the system line, the channel sent, and the
  lack of persistence already match v10.0.3.
- Test: `client/tests/chat_globe_e2e.py` (target `chat-globe-test`).
