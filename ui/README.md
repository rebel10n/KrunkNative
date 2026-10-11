# ui/ — the Krunker v3.1.1 menu as HTML/CSS

This is Krunker v3.1.1's own menu page, rendered by KrunkNative's in-house
HTML/CSS engine in the native client and by the browser in the web build (see
`doc/html-ui-engine.md`). Its behaviour is C (`client/src/ui/`), not JavaScript. The
goal is pixel parity with the real page in its loaded, logged-out, menu-open
state; everything Krunker shows a guest is shown here too, with the same markup,
`css/main.css`, fonts, icons and animations. Backend-driven content is shown the
way Krunker shows it to a guest ("Login or Register", level 1, "Loading
Streams...", "LOGIN REQUIRED" store/challenges/ranked, empty news/ads area).
It is hand-maintained source: edit it directly.

## Provenance

Imported from the v3.1.1 `public/` tree
(`/home/user/jails/kru/src/Krunker_v3.1.1/public`), then the window contents
were captured from the live page (offline server + headless Chromium). Only
files the menu references were copied (~2.7 MB): `index.html`,
`css/{main,flag,google-secular-one,material-icons}.css`, their fonts and
images, `img/menu-icons`, `img/social-buttons`, `img/maps/map_*.png` (host /
games window thumbnails), `img/{noimg,windows,mac,linux}.png`, `favicon.*`, the
few `textures/` the page uses and `textures/classes/icon_*.png`.
The Change Loadout pickers added `textures/previews/{weapons,cosmetics,attach}`
(only the default/base previews those windows show; every item's own preview
comes from the asset tree via `html_dom_asset_url()`, imported by `make assets-import`), `textures/reticles/reticle_0..11.png`, the
53 default `textures/sprays/*.png` of the Select Spray window, `img/ad_2.png`
(Advertise) and `windows/versions.txt` (v3.1.1 `docs/versions.txt`, the Changelog
source; kept next to the templates so the web build's preloaded package has it),
and `pro_{crosshair,scope,ads}.png` (served at the v3.1.1 site root; the Pro
settings preset points `crosshairImage` / `customScope` / `customADSDot` at them).
The in-game HUD and kill feed added `textures/weapons/icon_*.png` for every weapon
(`weaponConfig.icon`: icon_0..12 and icon_14..19, so the Crossbow, Famas, Sawed Off,
Auto Pistol, Bomb and Blaster rows are not blank) and the name-tag layer the CTF flags
and objective marker (`textures/{flag_r,flag_b,objective_1}.png`).

`tools/ui-import/menu-assets.txt` is the complete list of these byte-identical copies
(images, plus `css/main.css`, `css/material-icons.css`, the fonts and
`windows/versions.txt`); `make ui-import` (`tools/ui-import/menu_assets.py`) recopies
them from the v3.1.1 tree, so add a line there whenever C code starts generating a new
image path. The social hub's images are listed in `tools/ui-import/social_page.py`.
Known v3.1.1 404s, kept as in the original (the files do not exist in its `public/`):
`textures/previews/weapons/undefined.png` (Select Skin of the knife, the Runner's
primary), `img/modes/<mode>.png` (end screen Performance tab), `img/soldier_0.png`
(hub Clan Wars) and the `css/main.css` / `css/social.css` urls of
`img/{chals/icon_0.png,dropper_bg.svg,dropper_fg.svg,lock-icon-*.svg,world_0.png}`,
`img/social-buttons/fandom-*.png` and the web-archive `water_0`/`world_0`.
`KRUNKNATIVE_HTML_IMAGE_LOG=1|<file>` logs every image the native engine fails to load.

KrunkNative addition: `css/fonts/kn-symbols.ttf`, a subset (arrows, technical, box
drawing, blocks, geometric shapes, misc symbols, dingbats) of Adwaita Mono 34.1.0
Regular made with `pyftsubset --unicodes=U+2190-21FF,U+2300-23FF,U+2500-25FF,U+2600-26FF,U+2700-27BF,U+2B00-2BFF`
(SIL OFL 1.1, `css/fonts/kn-symbols-OFL.txt`). It is not referenced by the page: the
native engine uses it as its first per-glyph fallback (client/src/html/engine/text.c),
because Chrome draws these code points (e.g. the dev console's U+2715) from the
system's Adwaita Mono on the reference machine.

Transformations applied to the copied files:

- `?build=xxxx` query strings stripped; root-relative URLs made relative so the
  page works from `file://` and from any subpath.
- All original `<script>` tags (game JS, libs, captcha, ads, analytics), HTML
  comments, `og:` metas and external `<link>`s removed. External `<a href>`s
  neutralised (`href="#" data-ext-href=...`); the page never navigates.
- A `<canvas id="canvas">` added as the first body child (WebGL target in the
  web build; inert natively).
- `css/main.css` is untouched. `css/krunknative.css` only makes html/body
  transparent, positions `#canvas`, hides ad iframes / captcha, and turns
  pointer events off while the menu is closed. Menu open/close uses the same
  inline styles Krunker's `UI.toggleMenu()` sets.

## Our files

- `windows/` — the inner HTML of every menu window as the real game writes it
  (settings tabs General/Controls/Display/Render/Game/Sound, Account, Store,
  Challenges, Mod Manager, Ranked, Servers, Change Loadout, Select Class card,
  Custom Games item, Host Custom Game, Join, Change Controls, Client, Advertise,
  Support, Select Spray), split into
  one file per window plus `windows.json` by `make ui-import`
  (`tools/ui-import/`, source `tools/ui-import/krunker-windows.js`). The offline
  server's own additions ("Force Offline Mode", "Enable Developer Console") were
  removed. `client/src/ui/ui_windows.c` loads them and fills in class cards, map
  cards and settings values at run time.
- `css/krunknative.css`, `css/hud.css` — KrunkNative additions (transparent
  page over the scene, class-preview hole, the in-game HUD's crosshair and hit
  marker built from divs).
- `css/devconsole.css` + `css/fonts/lucon.ttf` — the v3.1.1 offline build's developer
  console (`mod/devConsole.ts` in the jail tree, not part of `public/`): its injected
  `<style>` copied verbatim (only the root-relative font URL made relative) and the
  `public/css/fonts/lucon.ttf` it loads. `client/src/ui/ui_console.c` adds the sheet at
  run time (`html_ui_add_stylesheet`) and builds the console's DOM the way the mod does;
  `client/src/ui/ui_mods.c` re-adds the mod's settings/controls rows (`mod/mods.ts`).
- `js/kn-shim.js` — **web build only**: forwards the markup's inline handlers
  and the few listeners to C (`kn_ui_event`) and lets `client/src/html/web_dom.c`
  address elements (`knId`/`knEl`). No UI logic.
- `js/loader.js` — **web build only**: loads `client.js` when served over HTTP.

The native client runs no JavaScript: its engine ignores `<script>`. What
Krunker's game JS did to this page is ported to C: `client/src/ui/menu_ui.c`
(`scaleBase()` for the 1700x900 design space times `uiCustomScale`,
`UI.toggleMenu/toggleMenuHider`, `updatePlayInstructions()` — `#instructions`
is exactly `CLICK TO PLAY` when connected, `LOADING...` / `CONNECTING...`
before, Krunker's error markup on a dead connection — `showWindow/closeWindow`,
the class box, `selectClass`), `ui_handlers.c` (every inline `on*` handler),
`ui_settings.c` (`setSetting(key, value)` with Krunker's own keys such as
`sensitivityX`, `fov`, `scaleUI`), `ui_loadout.c` (Change Loadout and its item
pickers, the K.P.D popup, the Changelog) and `hud_ui.c` (the in-game `#inGameUI`).

## Model viewer page (`viewer.html`)

Krunker v3.1.1's `public/viewer.html`, imported by `tools/ui-import/viewer_page.py`
(`make ui-import`): scripts removed (hook.js, dat.gui, zip.js, viewer.js), `?build=`
stripped, `css/dat-gui.css` and `css/viewer-kn.css` linked. `css/dat-gui.css` is the
stylesheet dat.gui 0.7 injects at run time (extracted unchanged from
`public/libs/dat.gui.min.js`, its data: URL folder arrows and `font` shorthands
included) and `css/viewer-kn.css` is ours (transparent page and `#container` over
the GL scene, the alert/prompt modal). The dat.gui panel, the scene, the URL parameters and the
imports are C: `client/src/viewer/` (the store tables in
`client/src/viewer/viewer_store_table.c` are generated from the v3.1.1
`viewer.js.map` by `tools/ui-import/gen_viewer_store.js`). Shown by
`client_bin --viewer "<query>"`, web `viewer.html?<query>` (its own URL), and links to
`/viewer.html` (`client/include/app_mode.h`).

## Map editor page (`editor.html`)

Krunker v3.1.1's `public/editor.html` ("Krunker Editor"), imported by
`tools/ui-import/editor_page.py` (`make ui-import`): scripts removed (hook.js,
quicksettings.js, pickr.min.js, editor.js and the inline `onbeforeunload` script), `og:`
metas dropped, `?build=` stripped, root-relative URLs made relative, `<canvas id="canvas">`
added as the first body child and `js/kn-shim.js` + `js/loader.js` before `</body>` (web
glue, as in `index.html`). `css/editor.css`, `css/editor_dark.css`,
`css/editor_custom.css` and `css/pickr.css` are byte-identical copies of the v3.1.1 files;
`css/editor-kn.css` is ours (transparent page over the GL view, the web build's
`#canvas`, and pointer-events so clicks on the 3D area reach it). Everything editor.js and libs/ObjectInstance.js /
TransformControls.js / ui/editor.js did is C: `client/src/editor/` (the prefab and config
tables in `client/src/editor/editor_data.c` are generated from the v3.1.1
`editor.js.map` by `tools/editor/gen_editor_data.py`). The panels (QuickSettings-style
blueprints) and the Pickr colour picker are rebuilt as Krunker's own markup by
`client/src/editor/editor_gui.c`. Shown by `client_bin --editor "<query>"` (`map=<name>`
opens `assets/maps/<name>.json`, `new` starts empty, otherwise the autosave in storage key
`knEditorMap` or the hub's `mapToEdit`), web `editor.html?<query>` (its own URL), and links
to `/editor.html` (`client/include/app_mode.h`).

## Social hub page (`social.html`)

Krunker v3.1.1's `public/social.html` ("Krunker Hub"), imported by
`tools/ui-import/social_page.py` (`make ui-import`): scripts (hook.js, recaptcha,
social.js, howler, hcaptcha, Chart.js, the inline captcha callbacks), comments and
description/og metas removed, `?build=` stripped, root-relative URLs made relative, the
asset-server profile icon pointed at the local `textures/classes/icon_0.png`, and the web
build's hidden `<canvas id="canvas">` + `js/kn-shim.js` / `js/loader.js` added as on
`index.html`. `css/social.css` and `css/social_custom.css` are byte-identical (social.css
keeps the dead web-archive `url()`s of the original, which fail on the real offline site
too); `css/flag.css`, `css/google-secular-one.css` and `css/material-icons.css` are the
menu's. Images: `img/{steam,android,ios,editor,black_m}.png`,
`img/{crossed_swords_0,shaking_hands}.svg`, `img/backgrounds/bg_test.png` and the Clan Wars
region rewards in `textures/previews/{cosmetics,melee}/`.

Its behaviour (src/social.js + src/ui/social.js of the v3.1.1 tree) is
`client/src/ui/social_ui.c`; the data comes from the game server over the social frames
of `shared/include/social.h`. Shown by `client_bin --hub "<query>"`, web
`social.html?<query>` (its own URL), the menu's Hub button and every `/social.html` link
(`client/include/app_mode.h`). Reference captures: `doc/ref/krunker-v3.1.1/hub/`.

## Behaviour notes

- Clicking the dimmed `#instructionHolder` plays (a click with a window open
  just closes the window); ESC closes a window, otherwise toggles the menu.
- The class preview is Krunker's `#classPreviewCanvas` (660x350 CSS px inside
  the scaled `#menuClassContainer`), left transparent. The UI logic reads its
  screen rectangle on setup, resize, menu open/close, class change and whenever
  its layout changes (resize observer) and the client renders the selected
  class's player model there.
- Guests cannot set a name in Krunker's chrome; the Account window's username
  field ("Login"/"Register") doubles as the nickname.
- Links that leave the page in Krunker (`Hub` = `social.html`, Discord, Terms,
  installers, ...) open in a browser tab: `window.open` on the web, the system
  browser natively (http(s) only, relative links against `https://krunker.io/`;
  `client/src/ui/ui_dialog.c`). Krunker's `confirm()` / `prompt()` / `alert()`
  are the browser's on the web and the page's own `#confPop` popup natively.
- Map selection happens through the Host Game window's map cards and the Custom
  Games list.
- Interaction parity (every button/window, real vs native vs web):
  `doc/ref/krunker-v3.1.1/interactions/README.md`.

Preview: `make ui-test ARGS="render-page ..."` renders the page with the native
engine to a PNG (`--menu-preset` applies the menu-open state); `make web` +
`make web-serve` shows it in a browser with the full C UI logic. Opened from
`file://` the page is static (no windows, no menu state).
