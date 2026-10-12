# Krunker v7.1.4 map editor reference captures

Captures of the REAL Krunker v7.1.4 map editor (its own `public/editor.html` and bundled
`js/editor.js`) in headless Chromium at 1920x937 (`#uiBase` scale 1), the parity reference of
`doc/v7/editor.md` (WP0, "Reference capture"). Structural dumps are the primary check: they use
the same format as the native editor's `KRUNKNATIVE_EDITOR_SCRIPT="dump:<which>:<file>"`
(`client/src/editor/editor.c`), so a native panel or window can be diffed against v7 as text.
The PNGs are secondary (`tools/uidiff` scenarios `editor-*`); Chromium and the native engine
rasterise fonts differently. Everything here is regenerated, never hand-edited.

| Files | State |
|---|---|
| `editor-1920.{png,html.gz,rects.json}` | the editor after `closeWindow()` (the Welcome window shown at boot closed), new map, nothing selected |
| `toolbar.dump.txt` | the whole `#_toolbar` menu tree |
| `toolbar-<root>.png`, `toolbar-object-{basic,models,lights}.png` | each root menu opened by a mouse hover; the Object sub-menus |
| `window<N>.{png,dump.txt,rects.json}` | `showWindow(N)`, N = 1..15 |
| `window1-advanced`, `window3-{display,object,quickgui}`, `window4-edit` | the other Controls / Settings tabs, Quick Add in Customize mode |
| `teleport.{png,dump.txt}` | `KE.teleportPopup()` (`#genericPop`) |
| `left-essential.dump.txt` | the left panel (map config) |
| `right-<name>.{png,dump.txt}` | `GUI.panel.right.<name>.show()`: environment, terrain, zone, ocean, tools, prefab, objectList, gui, blank |
| `logic/<case>.json`, `logic/<case>.dump.txt` | `tools/editor/logic_cases.json` replayed by GUI.action on the panel blueprints (editor WP4): a trigger case's object `serialize()` and right panel, a GUI / config case's `{config, gui}` of `getMapExport()` and its right / left panel; captured without `js/hook.js` (below) |
| `tools/<case>.json` | `tools/editor/tools_cases.json` (editor WP6): the map imported, `KE.optimizeFaces()` or `KE.mergeColors()` (`colorRange` = the case's range); `{alert, before, after}` with every object's `serialize()` (`tools/editor/tools_check.py`) |
| `net/<popup>.{png,dump.txt}` | the publishing / My Games popups (editor WP6) with a faked free `KE.account`: `publishGamePopup(true)`, `publishGamePopup(false, row)`, `publishAssetPopup()` (Model and Sound), `openEdit`, `mapResp`, `deleteCreationConf` (`tools/editor/windows_check.py`) |
| `prefabs/<id>.json`, `prefabs/<id>.right.dump.txt` | after `KE.clearMap()`: `KE.createObjects[id]()`, the new object's `serialize()` and the right (object) panel; `prefabs/skipped.txt` lists the ids not created; captured without `js/hook.js` (below) |

## Dump format

`text(e)` is `textContent` cut to 511 characters, whitespace runs collapsed to one space,
trimmed, and cut to the native buffer sizes; a missing attribute prints `null`. The element
children of the root are walked:

- `.panelLabel` -> `P <text>`
- `.header` / `.subheader` / `.subsubheader` -> `F` / `Fs` / `Fss` `<text(a)> open|closed`
- `.inlineItem` -> `L[<classes of .section minus "section">] <text(.section)> : <controls>`; a
  control is `switch=0|1`, `range[min,max,step]=value`, `sval=value`, `color=value`,
  `<type>(<placeholder>)=value`, `select#<options>[<first>..<last>]=<selected>`,
  `button:<text>` (`.button`, and v7's `.eButton` / `.dbutton`) or `label:<text>`
- `.vector3` -> `V value/min,value/min,value/min`; `hr` -> `-`; `.section` -> `S <text>`;
  a div whose only element child is a `.button` / `.eButton` -> `B <text>`
- windows and popups only: `.menuTabNew` / `.menuShortTabNew` -> `T <text>` (` *` = active tab),
  `input` / `select` -> `I <control>`, a leaf with text -> `X <text>`, any other element's own
  text nodes -> `X <text>` before its children

Window dumps start with `H <text(#windowHeader)>`. The toolbar dump is the menu tree: `M <label>`
(a menu, children indented by two spaces), `C` (one 12-row column of a chunked sub-menu),
`O <item>`, `-`.

## How they were made

`node tools/editor/v7ref.js [--out <dir>] [--list] [state...]` (about 90 s for everything). The
reference tree is never written to and v7's npm server is never run: the tool serves
`/home/user/jails/kru/src/Krunker_v7.1.4/public` read-only on a free 127.0.0.1 port
(`tools/ui-import/v7_page.js`) and drives `chromium-browser --headless=new` (swiftshader WebGL)
over CDP (`tools/kref/cdp.js`) on another free port. Determinism:

- no network but the static serve: `https://*` and `wss://*` are blocked (the editor socket,
  captcha, the YouTube iframes of windows 12 and 13, which show Chromium's error page), so no
  "Error: DISCONNECTED" notice appears in `#chatList`;
- empty `localStorage` (the default `kro_editor` settings), a seeded `Math.random` and
  `alert` / `confirm` / `prompt` stubs installed before any page script;
- the hook's session timer box removed, CSS animations paused at 800 ms;
- the Welcome window (8) is captured once its counters reach 160,000,000 / 12 / 750,000.

The page's host is not `127.0.0.1:8080`, so v7's `config.isProd` is true, as on krunker.io: the
`localOnly` prefabs (40 EVENT, 41 TERMINAL, 95 BOT_SPAWNER) are neither listed in the menus nor
created, and the Event Objects counter stays hidden. 44 CUSTOM_ASSET opens the asset browser
and has no object. The `prefabs` and `logic` states load the page without the served
`public/js/hook.js`: it defines an `Object.prototype.events` accessor, so `vO234.events = {...}`
(UI:4081) never becomes an own key of the Technical blueprint and `_build` skips the "Trigger
Event" folder of TRIGGER (29) and AI_SPAWNER (55). Without the hook the folder (Can Trigger, Event,
Event Value) shows between Hitbox and Trigger Actions, as in the real editor and the native panel
(`client/src/editor/editor_triggers.c`). Windows that need Krunker's API (2 Assets, 5 Mods, 7 Textures, 9 Sounds,
14 Maps) show their offline `Loading...` state.
