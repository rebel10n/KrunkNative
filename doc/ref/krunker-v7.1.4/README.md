# Krunker v7.1.4 reference captures

Screenshots, DOM dumps and element rects of the REAL Krunker v7.1.4 client (its own
`public/index.html` / `social.html` and bundled `js/index.js`), headless Chromium at 1280x720
unless noted. They are the acceptance reference for KrunkNative's HTML/CSS UI: `make uidiff`
renders the scenarios of `tools/uidiff/scenarios/` with the native client and diffs them against
these images. The v3.1.1 references stay in `doc/ref/krunker-v3.1.1/`.

- `menu/`: the loaded menu of a returning guest (`menu-1280`, `menu-1920`).
- `menu/menu-disconnected-{1280,1920,1024}`: the Lost Connection screen (io_error's UI steps and
  `f2040` from the bundle scope: the error box alone, `#menuHolder` and the chat hidden).
- `windows/`: menu windows (`win-*`): Game Settings tabs (advanced view, plus `win-settings-basic`),
  Servers, Change Loadout (Loadout and Class tabs), Mod Manager, Account, Change Controls, Host
  Custom Game, Select Spray, Custom Games, Store, Join, Client, Ranked, Select Attachment / Reticle,
  Challenges, Changelog, the Host Game and Server Finder type pickers.
- `ingame/`: HUD states in v7's sandbox game (`ingame-*`): plain, scoped, reloading, scoreboard
  (Tab), chat open, kill feed, death screen (killed by a bot / no killer), end screen, ESC pause;
  and (WP-E part 2a) `ingame-death-v7` (the death screen with the killer's class level, player card
  stat and the life's stats, set on the sandbox server's players before its `players.kill`),
  `ingame-spectate` (`setSpect(true)` before the pointer lock: the free cam's `#spectateUI`) and
  `ingame-minimap` (v7's `customsData` cleared and `toggleMinimap(true)`: production hides the
  minimap on custom maps, and the captures load Kanji as one).
- chat at other window sizes (menu-ux): `menu/menu-1024`, `menu/menu-tall` (720x1280),
  `ingame/ingame-chat-{1024,1920,tall}` (a bot spawned: one chat line), `ingame/ingame-pause-{1024,tall}`
  (ESC after that), `ingame/ingame-spectate-menu` (ESC while spectating) and `ingame/ingame-chat-lines`
  (12 spawnBot / clearBots lines: `#chatList` at its max-height, scrolled to the newest).
- `hub/`: the social hub (`hub-*`) as a guest: views loading, login and settings popups.
- `viewer/`: the model viewer `public/viewer.html?<query>` at 800x600 (WP-F): showcase item views,
  cosmetic render pages, a player wearing every slot and `dev-*` edits through the dev config
  folders (dat.gui actions after the load), with the clock frozen and only
  `#container` shown (`tools/kref/viewer-ref.js`, scenarios in `tools/kref/viewer-scenarios.txt`,
  recaptured with `REFS=1 tools/kref/viewer-diff.sh`). `make viewer-diff` renders the same queries
  with `client_bin --viewer` (headless, `KRUNKNATIVE_VIEWER_FREEZE=1`) and checks the RMSE.

Each capture is `<name>.png`, `<name>.html.gz` (the DOM after the capture) and `<name>.rects.json`
(viewport rects of the page elements `tools/uidiff/gen_scenarios.py` builds the regions from).

## How they were made

`node tools/uidiff/capture-v7.js [name...]` (`--list` names the scenarios). Nothing in the
reference tree is modified and v7's npm server is never run (it writes into the tree): the tool
serves `/home/user/jails/kru/src/Krunker_v7.1.4/public` read-only on 127.0.0.1 and drives
Chromium (`chromium-browser --headless=new --hide-scrollbars`, swiftshader WebGL) over CDP. With no
matchmaker the page starts v7's in-browser sandbox game. Determinism:

- storage before the page loads: `krunker_haslogin` (a returning player: v7's `fullMenu()`, no
  FTUE / bot match), consent, default settings, and `custMap` = `public/maps/kanji.json` with its
  config narrowed to Free for All, so the sandbox is always FFA on Kanji (`loadSandbox`);
- a seeded `Math.random` installed before any page script, and v7's reconstruction slip
  `requestAnimationFrame(fn, delay)` (a decompiled `setTimeout`: death screen delay, copyGameLink,
  the end-screen results) run as `setTimeout`, as in the original game and in KrunkNative;
- the menu undoes the sandbox chrome (`CONNECTED_MENU` of `tools/ui-import/v7_page.js`: the
  connected guest menu), shows the stream panel (`Loading Streams...`) instead of the ad slot,
  pins `#mainLogoFace` to `f_0` and removes the hook's session timer box;
- the 3D canvas is transparent (opacity 0, it still takes the mouse) over a flat page background:
  `#d6c6c6` for the menu and windows, `#000` in game, matching the scenarios' `background`; the
  class preview canvas is left as is (its region is skipped);
- every CSS animation is paused at 800 ms (`document.getAnimations()`, `currentTime = 800`); the
  scenarios use `anim_time` 0.8;
- in game: v7's real pointer-lock path (a `requestPointerLock` shim sets `pointerLockElement` and
  fires `pointerlockchange`: `enterGame()` + `UI.toggleControlUI(true)`), the match timer pinned to
  03:45, real CDP input for Tab / R / Enter / right mouse, `pressButton(107)` (v7's spawnBot bind)
  for a bot, `sandboxGame.players.kill(victim, killer, {})` (the sandbox server) for the kill feed
  and the death screens, `sandboxGame.endGame()` for the end screen;
- Servers: `client.fetchGameList` / `fetchGameInfo` answer three games in us-ca-sv, us-ca-la and
  us-il (the matchmaker's tuple format), so the window shows its region groups;
- hub: the social socket is a WebSocket stub that opens and never answers (there is no social
  server): the guest chrome with every view loading.

Not captured: the end-screen results that follow the end animation (`io_endGame` only shows them
with a connected socket; the sandbox's own end deactivates the local player first), hub views with
data, the hit marker frame (drawn on the overlay canvas with the real clock).
