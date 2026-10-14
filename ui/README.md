# ui/ — the Krunker v10.0.2 menu as HTML/CSS

This is Krunker v10.0.2's menu page (krunker.io, mirrored 2026-10-13), the only page Krunker Box
Office has (Krunker v10.0.2 is the only version: doc/v7/decisions.md), rendered by the in-house
HTML/CSS engine in the native client and by the browser in the web build (see
`doc/html-ui-engine.md`). Its behaviour is C (`client/src/ui/`: `v10_*.c`, `hud_v10.c` and the
older modules), not JavaScript. Until 2026-10-14 it was the separate tree `ui/v10/` next to the
v7.1.4 page; that page was retired (git history keeps it, and the v3.1.1 page is on the `v3.1.1`
branch/tag). The page started as an import (`make ui-import`, `tools/ui-import/`) and has been
edited by hand since for v10 parity (the v10 area work): a re-import is a starting point to diff,
not a replacement.

## Provenance

The page was imported by `python3 -I tools/ui-import/import.py --ui ui` (`tools/ui-import/v10_page.py`;
options: `tools/ui-import/README.md`) and edited since. Which source each file comes from:

| File(s) | Source |
|---|---|
| `index.html` | Byte copies of the **real served** pages (`https://krunker.io/`, mirror `site.lock`), with the v7 rules applied: scripts, comments, og/twitter/description/apple metas (`platform/web/pwa.py` writes ours), preload/PWA links and `?build=` removed, URLs relativised, external links neutralised, canvas + our css/js added. |
| the markup inside `index.html`'s `#mainMenuUIMount`, `#notificationCenter`, `#gameAnnouncementEl`, `#matchmakingStatusEl`, `#matchEndSignupIncentives` | **Reconstructed from template strings**, not the rendered page. v10's menu is Svelte 5, so the served `index.html` has empty mount points. `tools/ui-import/v10_bundle.js` parses the client script (never runs it) and plays each boot component's mount code over a toy DOM in its initial, logged-out state (unknown runtime state reads as falsy). Expect the gaps of a static reading: lists built from server data (streams, popular games, news) are empty, and elements v10 shows or hides at run time keep their initial inline style. |
| `css/svelte.css` | **Reconstructed**: the 212 component stylesheets (`{hash, code}` strings of the bundle) that the Svelte runtime injects as `<style>` at mount, the mounted ones first. URLs moved one directory down. |
| `i18n/en.json` | **Reconstructed**: the bundle's inline `translations.en` (2170 keys; `i18n-en.json` until 2026-10-14). |
| `i18n/<code>.json` (es de nl kr pr jp fr fi zh zhtr pl hi ru it tg tr) | Byte-for-byte content of the **served** `https://krunker.io/i18n/<code>.js?v=i41Nu` (what v10's `getTranslations` loads), fetched 2026-10-14 and converted from v10's "key value" line format to JSON by `tools/i18n-import/i18n_import.py`. Details, `i18n/kn/` (Krunker Box Office's own strings) and the gaps: `i18n/README.md`. |
| `css/fonts/kn-noto-*` | Ours: Noto Sans CJK JP / SC / TC / KR and Noto Sans Devanagari subsets (OFL 1.1, `css/fonts/kn-noto-OFL.txt`) the native engine falls back to for the shipped languages (`i18n/README.md`, "Fonts"). |
| `css/*.css` (Krunker's), `css/fonts/*`, `img/`, `textures/`, the other images | Byte copies of the served files (URL rewriting only). Each `.woff2` also gets a `.ttf`, and each `.webp` a `.webp.png` sidecar (the engine decodes neither). Files the live hosts 404 on (the Play subsets, `favicon.png`, `pro_scope.png`, ...) are v7.1.4's copies (the retired page's). |
| `windows/versions.txt` | The served `docs/versions.txt` (changelog). |
| `windows/` (`<n>.html`, `settings-{basic,advanced}-<t>.html`, `controls-<t>.html`, `windows.json`) | **Rendered**: what v10's own client writes into `#menuWindow` for `showWindow(1..54)`, every settings tab of both views and the five Change Controls tabs, captured by `capture_v10_live.js` (offline mode, below) and kept as `tools/ui-import/krunker-windows-v10.json.gz`; split by `tools/ui-import/windows_v10.py` with `windows_v7.py`'s rules (pickers cut to 4 cards, per-weapon setting clones and the host window's big Advanced bodies dropped, Download Client = ours). Windows 5 (Account) and 14 (Store) are v7.1.4's captures (`"from": "v7.1.4"` in windows.json): offline, v10 shows a guest its login popup instead of Account and cannot build the Store without its API. Slots 11, 12, 22, 28, 29 are empty in v10. The `#classPreviewCanvas` v10 moves into the loadout windows is dropped from the templates (the page keeps the menu's one). |
| `img/kn-strike-logo.png`, `css/fonts/ChakraPetch-OFL.txt` | Ours (Krunker Strike, `doc/v10/krunker-strike.md`): the real Krunker Strike FRVR logo (`https://krunkerstrike.frvr.com/img/krunker_logo.png`, 1024x348, pinned in `tools/ui-import/live_assets_v10.lock`) that `client/src/ui/flavor_ui.c` puts in place of `#mainLogo` when the Strike flavor is active, and the OFL 1.1 licence of `css/fonts/ChakraPetch-Bold.ttf` (Cadson Demak's Chakra Petch; Krunker v10 serves it, the Strike page uses its 700 weight, `css/krunknative.css` declares the `@font-face`). A base build never shows them. |
| `css/krunknative.css`, `css/hud.css`, `css/devconsole.css`, `css/ranked-kn.css`, `js/kn-shim.js`, `js/loader.js` | Ours. |
| `windows/kn-versions.txt`, `css/kn-v10-news.css` | Ours: Krunker Box Office's changelog, which the What's New window shows (Krunker's `docs/versions.txt` format, newest update first = the version), and the native fill-ins of the What's New / Guide / Community popups (`client/src/ui/v10_news.c`, doc/v10/ui.md). |

Rendered page: `node tools/ui-import/capture_v10_live.js <stage>` (run with the user's approval:
it runs Krunker's client) captures the booted DOM, every `<style>`/adopted sheet, the popups
(ranked, Turf Wars, leaderboards, guide, community), settings/customize, `showWindow(1..54)` and
the settings / controls tabs into `<stage>/v10-capture/`, in sandboxed headless Chromium (an
allow-list of krunker.io hosts, GET/HEAD only, no WebSocket). On 2026-10-13 krunker.io answered
this machine with Cloudflare's "ACCESS DENIED" (403 on every path, also from headless Chromium),
so the windows were captured in its offline mode (`KN_V10_PUBLIC=<mirror>/public`): the same
page and pinned client script (`~/.cache/kn-v10/v10.js`, the 2026-10-13 v10.0.2 bundle) answered
from the mirror with nothing leaving the machine, FRVR's SDK replaced by an inert logged-out
stand-in, and no API (store data, server lists, news). Files taken from it are marked **rendered**.

What the native engine does not draw on this page. None of these is used by the window
templates, and a headless render of every `showWindow(1..54)`, settings tab and controls tab
on this page shows no blank window (`make smoke-ui` renders a subset); the stylesheet rules
that use them degrade as follows: `backdrop-filter` (frosted panels of Svelte popups: Turf Wars
territory panel, shop header, premium cards, notifications) is ignored, so those panels show
their plain translucent background; `aspect-ratio` (`.eventBanner`, market / trade / profile
thumbnails) is ignored, so those boxes size from their content or explicit height; `-webkit-
line-clamp` (`.eventDesc`, `.pollDesc`) is ignored, so event text is not cut at its line count;
`mask-*` (`.menuItemIconMask` icons) is ignored, so a masked icon paints as a `currentColor`
square; `color-mix()` (`.chalRewTile`, `.titleRow`, `.titleSlotTag`, `.leaderTitle` gradients /
border) is not parsed, so those declarations are dropped and the tiles lose that tint.
Images the templates name that neither the mirror nor the retired v7.1.4 page had (broken images, as on v10
without its CDN): `img/mouse/10002.png` (controls), `img/backgrounds/gamecats/c_0.png` /
`c_9.png` (host / server type), `textures/previews/weapons/weapon_21.png`,
`textures/previews/cosmetics/12_default.png`, and `css/svelte.css`'s `img/lottery_bg*.webp`.

## Our files

- `css/krunknative.css` — Krunker Box Office's page overrides: transparent page over the scene,
  the class-preview hole, ads hidden, pointer events while the menu is closed, and the map
  terminal / interact progress bar markup v7 dropped (Krunker v3.1.1's rules; the elements
  are created by `client/src/ui/ui_terminal.c`).
- `css/hud.css` — the in-game HUD's crosshair and hit marker built from divs (v7 draws them on
  its overlay canvas).
- `css/ranked-kn.css` — the Ranked window (slot 27) in Krunker v10.0.2's layout and the Turf Wars
  window (v10 shows both as Svelte popups whose styles live in its JS bundle), added at run time
  by `client/src/ui/ranked_ui.c` / `turf_ui.c` (doc/v10/ranked-impl.md).
- `img/ranked/ranks/rank_*.svg` — v10.0.2's eight rank icons, unchanged from
  `assets.krunker.io/img/ranked/ranks/` (the ranked window, `shared/src/ranked.c` ranked_rank_icon).
- `css/devconsole.css` + `css/fonts/lucon.ttf` — the offline build's developer console
  (`mod/devConsole.ts` in the jail tree): its injected `<style>` (only the root-relative font
  URL made relative). `client/src/ui/ui_console.c` adds the sheet at run time
  (`html_ui_add_stylesheet`) and builds the console's DOM the way the mod does;
  `client/src/ui/ui_mods.c` re-adds the mod's settings/controls rows.
- `css/editor-kn.css`, `css/viewer-kn.css`, `css/dat-gui.css` — see the editor / viewer pages.
- `js/kn-shim.js` — **web build only**: forwards the markup's inline handlers and the few
  listeners to C (`kn_ui_event`) and lets `client/src/html/web_dom.c` address elements
  (`knId`/`knEl`). No UI logic.
- `js/loader.js` — **web build only**: loads `client.js` when served over HTTP.

The native client runs no JavaScript: its engine ignores `<script>`. What v7's game JS
(src/index.js, src/libs/UI.js, src/libs/settings.js, src/ui/game.js) did to this page is
ported to C in `client/src/ui/`: `menu_ui.c` (`scaleBase()` for the 1700x900 design space
times `scaleUI` (0.7 by default), `UI.toggleMenu/toggleMenuHider/toggleWindow`,
`updatePlayInstructions()` — `CLICK TO PLAY` / `CLICK TO SPECTATE` when connected, v7's
loading ring before, the "Lost Connection" screen in `#instructionsUpdate` on a dead
connection — `showWindow` with each window's class and inline style, `updateClassPreview()`,
`mapInfo`), `ui_handlers.c` (the core inline handlers and the module handler chain),
`ui_windows.c` (window templates and generators), `ui_settings.c` / `ui_controls.c` (Game
Settings and Change Controls), `ui_loadout.c` (Change Loadout and its pickers), `store_ui.c`,
`account_ui.c`, `clan_ui.c`, `ranked_ui.c`, `servers_ui.c` (Servers, Server Finder, Custom
Games, Join, Connect) and `hud_ui.c` (the in-game `#inGameUI`, death and end screens).

Krunker Box Office additions on the v7 page (each restyled with v7's own classes): the menu bar's
"Ranked" button after Server Finder (v7 keeps the window and its `menu.btn.ranked` label but no
button), the "Connect" button next to Invite/Join (direct connect to `host[:port]`,
default port 21015, the console `connect` path), the "Exit" item and Settings > Client tab of
the desktop client, the Settings rows of the client mods (bhop, dev console binds), the
Audio Output row, the Lag Compensation slider, the map terminal and interact bar, and the
"Play Offline" tab on the connection error screen.

## Model viewer page (`viewer.html`)

Krunker v7.1.4's `public/viewer.html` (the same markup as v3.1.1's), imported by
`tools/ui-import/viewer_page.py` (`make ui-import`): scripts removed (hook.js, dat.gui, zip.js,
fflate, viewer.js), `?build=`
stripped, `css/dat-gui.css` and `css/viewer-kn.css` linked. `css/dat-gui.css` is the
stylesheet dat.gui 0.7 injects at run time (extracted unchanged from
`public/libs/dat.gui.min.js`, its data: URL folder arrows and `font` shorthands
included) and `css/viewer-kn.css` is ours (transparent page and `#container` over
the GL scene, the alert/prompt modal). The dat.gui panel, the scene, the URL parameters and the
imports are C: `client/src/viewer/` (the store tables in
`client/src/viewer/viewer_store_table.c` are generated from v7.1.4's store by
`tools/ui-import/gen_viewer_store.js`; `import.py --viewer-store` reruns it). Shown by
`client_bin --viewer "<query>"`, web `viewer.html?<query>` (its own URL), and links to
`/viewer.html` (`client/include/app_mode.h`).

## Map editor page (`editor.html`)

Krunker v10's live `editor.html` (krunker.io, mirrored 2026-10-13 by `fetch_live_site.py`), imported
by `tools/ui-import/v10_hub_editor.py` (`make ui-import`): every script removed (FRVR SDK, the inline
FRVR config, gtag, ld+json, the Vite module entry, altcha, CodeMirror, Tween, howler,
quicksettings, pickr, jszip, fflate), comments, preload links and description / og / twitter metas
dropped, `?build=` stripped, URLs made relative, the cdnjs CodeMirror 5.61.0 CSS pointed at
`libs/codemirror/5.61.0/`, the shared Vite stylesheet `static/index-<hash>.css` written as
`static/index.css`. `css/editor.css`, `css/editor_dark.css`, `css/editor_custom.css`,
`css/pickr.css` are v10's (URL rewrite of the v7 import). KrunkNative additions kept on every
import: `<canvas id="canvas">` (first body child), `js/kn-shim.js` + `js/loader.js`,
`css/editor-kn.css` (transparent page over the GL view, pointer-events, the `#knPrompt`
dialog), `css/editor-script.css` (the KrunkScript window), `css/editor-ai.css` and the AI
button `#knAiOption`, and v7.1.4's 2-Factor popup `#twoFAPop` + `css/kn-twofa.css` (v10 dropped
it; KrunkNative keeps 2FA logins). Parity notes: `doc/v10/editor-social.md`.
Everything editor.js and libs/ObjectInstance.js / TransformControls.js / EditorOverlay.js /
ui/editor.js did is C: `client/src/editor/` (the prefab and config tables in
`client/src/editor/editor_data.c` are generated from v7's `src/` plus the v10 appendix by
`tools/editor/gen_editor_data.py`). The panels (QuickSettings-style blueprints) and the Pickr
colour picker are rebuilt as Krunker's own markup by `client/src/editor/editor_gui.c`; the
`#editor-overlay` canvas (FPS counter) is drawn with Canvas2D (`editor_overlay.c`). Shown by
`client_bin --editor "<query>"` (`map=<name>` opens `assets/maps/<name>.json`, `new` starts
empty, otherwise the autosave in storage key `knEditorMap` or the hub's `mapToEdit`; the
Welcome window opens unless a map came with the page), web `editor.html?<query>` (its own
URL), and links to `/editor.html` (`client/include/app_mode.h`). Reference captures:
`doc/ref/krunker-v7.1.4/editor/` (`tools/editor/v7ref.js`).

## Social hub page (`social.html`)

Krunker v10's live `social.html` ("Krunker Hub - Gaming Community, Trading & Social Features"),
imported by `tools/ui-import/v10_hub_editor.py` (`make ui-import`), with the editor page's rules:
scripts, comments, preload links and description / og / twitter metas removed, `?build=`
stripped, URLs made relative, `static/index.css` linked, the web build's hidden
`<canvas id="canvas">` + `js/kn-shim.js` / `js/loader.js` added, the app badge pointed at our
Download window, `#twoFAPop` + `css/kn-twofa.css` kept, and `css/hub-svelte.css` (v10's view
stack / popup / LoginModal component CSS, from `svelte.css` of `v10_bundle.js`) linked.
`css/social.css` and `css/social_custom.css` are v10's (URL rewrite).

Its behaviour (v10's `static/social-*.js`, read through webcrack; v7.1.4's src/social.js) is
`client/src/ui/social_ui.c`; the data comes from the game server over the social frames
of `shared/include/social.h`. Shown by `client_bin --hub "<query>"`, web
`social.html?<query>` (its own URL), the menu's Hub item and every `/social.html` link
(`client/include/app_mode.h`). v7 removed the Clan Wars and Tournament pages. Reference
captures: `doc/ref/krunker-v10/hub/` (reconstructed, `tools/uidiff/capture-v10-static.js`).

## Behaviour notes

- Clicking the dimmed `#instructionHolder` plays (a click with a window open
  just closes the window); ESC closes a window, otherwise toggles the menu.
- The class preview is Krunker's `#classPreviewCanvas` (750x600 CSS px inside
  the scaled `#menuClassContainer`). The UI logic reads its screen rectangle on
  setup, resize, menu open/close, class change and whenever its layout changes
  (resize observer); the client renders the selected class's player model at
  the box's device-pixel size and the page shows it as the canvas's bitmap
  (`html_dom_canvas_gl`), transparent around the model, so the canvas stays in
  the page's paint order (above `#instructionHolder`'s shade, under popups).
- Guests cannot set a name in Krunker's chrome; the Account window's username
  field ("Login"/"Register") doubles as the nickname.
- Links that leave the page in Krunker (`Hub` = `social.html`, Discord, Terms,
  installers, ...) open in a browser tab: `window.open` on the web, the system
  browser natively (http(s) only, relative links against `https://krunker.io/`;
  `client/src/ui/ui_dialog.c`). Krunker's `confirm()` / `prompt()` / `alert()`
  are the browser's on the web and the page's own `#genericPop` popup natively.
- Map selection happens through the Host Game window's map cards and the Custom
  Games list.
- Reference captures of the real v10.0.2 page (and of the retired v7.1.4 one) and the uidiff scenarios:
  `doc/ref/krunker-v10.0.2/`, `doc/ref/krunker-v10/`, `doc/ref/krunker-v7.1.4/README.md`, `tools/uidiff/README.md`.

Preview: `make ui-test ARGS="render-page ..."` renders the page with the native
engine to a PNG (`--menu-preset` applies the menu-open state); `make web` +
`make web-serve` shows it in a browser with the full C UI logic. Opened from
`file://` the page is static (no windows, no menu state).
