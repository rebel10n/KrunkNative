# tools/ui-import

One-shot (but idempotent) importer that turns the Krunker v3.1.1 UI sources
into the static inputs the native HTML engine consumes. The Makefile target
`make ui-import` runs:

```sh
python3 -I tools/ui-import/import.py --ui ui
```

Outputs are committed to the tree; re-running only rewrites files whose
content changed.

## What it produces

| Output | Source | Notes |
|---|---|---|
| `ui/css/fonts/material-icons.ttf` | every `.woff2` referenced by an `@font-face` in `ui/css/*.css` that exists (today only Material Icons; the Secular One files are missing and skipped) | file name = slug of the rule's `font-family`; an existing loadable `.ttf` is kept unless `--force` |
| `client/src/html/engine/material_icons_liga.h` | GSUB ligature lookups of the Material Icons woff2 (Google serves the font with `rlig`) | `MaterialIconLiga material_icons_liga[MATERIAL_ICONS_LIGA_COUNT]` = `{name, codepoint, glyph_index}` sorted by name in byte order (+ `MATERIAL_ICONS_LIGA_MAX_NAME`); the format of `gen_liga.py` / what `engine/text.c` consumes |
| `ui/windows/<id>.html`, `<id>-card.html`, `<id>-item.html`, `<id>-mapcard.html`, `settings-head.html`, `settings-tab-0..5.html` | `tools/ui-import/krunker-windows.js` (`window.KRUNKER_WINDOWS = {...}`; a `ui/js/krunker-windows.js`, if one is put back, takes precedence) | byte-identical HTML strings, UTF-8 |
| `ui/windows/windows.json` | same | `{"windows": {"<id>": {"header", "sticky", "file", "card"?, "item"?, "mapCard"?, "itemCount"?}}, "settings": {"header", "sticky", "head", "tabs": [6]}}` |
| `tools/ui-import/krunker-windows.js` | archived copy of the former `ui/js/krunker-windows.js` (deleted from `ui/js/` at the engine cutover) | the source of the window templates; also read by the engine tests in `client/src/html/engine/tests/` |

`gen_liga.py` is the standalone ligature/TTF generator (`--font --header [--ttf]`);
`import.py` loads it when present so both emit byte-identical headers, and
falls back to an equivalent built-in writer otherwise. Saved TTFs use
`SOURCE_DATE_EPOCH=0` (unless set) so re-conversions are reproducible.

## Menu and HUD images

`menu_assets.py` (step of `import.py`, skip with `--no-assets`) copies every file listed
in `menu-assets.txt` from the v3.1.1 `public/` tree into `ui/` at the same path,
byte-identical: every image the menu page, the window templates and the C UI logic
(`client/src/ui/`, `client/src/nametags.c`) reference, plus the other byte-identical site
files (`css/main.css`, `css/material-icons.css`, fonts, `windows/versions.txt =
docs/versions.txt`). `{a..b}` and `{x,y}` expand like shell braces; a listed file the
reference does not serve stops the import. Add a line when code starts generating a new
image path (audit: `KRUNKNATIVE_HTML_IMAGE_LOG=<file>` while walking the UI natively).

## Game content mod.zip lacks

`game_assets.py` (`make assets-import`, not part of `import.py`: `assets/` is not committed)
copies whole directories of the v3.1.1 `public/` tree into `assets/`, byte-identical:
`textures/previews/` (attach, cosmetics, melee, weapons), the item previews
`UTILS.getPreview(skin, store)` loads from Krunker's asset host
(`assetsUrl("/textures/previews/" ...)`). The C UI writes their URLs with
`html_dom_asset_url()` (native `/assets/...`, read from `assets_path()` by the engine; web the
asset host's versioned URL), so they are not copied into `ui/`. `--check` lists files that are
missing or differ. Deploy: `ansible-playbook central.yml --tags web` (role kn_web syncs
`assets/` and rehashes `assets/version.txt`).

## Model viewer page

`viewer_page.py` (step of `import.py`, skip with `--no-viewer`) writes
`ui/viewer.html` (Krunker's `public/viewer.html`, scripts removed) and
`ui/css/dat-gui.css` (dat.gui's injected stylesheet), and runs
`node gen_viewer_store.js [viewer.js.map] [out.c]`, which regenerates
`client/src/viewer/viewer_store_table.c` (every `store.skins` entry, rarities,
previews, skin colours and the viewer's dropdown option lists computed with
viewer.js's own sorting) from the sources embedded in the v3.1.1 `viewer.js.map`.

## Social hub page

`social_page.py` (step of `import.py`, skip with `--no-social`) writes `ui/social.html`
(Krunker's `public/social.html`: scripts, comments and description/og metas removed,
`?build=` stripped, root-relative URLs made relative, the asset-server profile icon
pointed at `textures/classes/icon_0.png`, plus the web build's hidden `#canvas` and
`js/kn-shim.js` / `js/loader.js` like `index.html`), copies `css/social.css` and
`css/social_custom.css` byte-identical, the images the page and `client/src/ui/social_ui.c`
use (`img/{steam,android,ios,editor,black_m,favicon,noimg}.png`,
`img/{crossed_swords_0,shaking_hands}.svg`, `img/backgrounds/bg_test.png`) and the eight
Clan Wars region reward previews under `textures/previews/`.

## Dependencies

`fonttools` and `brotli` must be importable under `python3 -I` (isolated mode
ignores user site-packages), i.e. installed system-wide:
`dnf install python3-fonttools python3-brotli` or
`sudo pip install fonttools brotli`.

## Options

```
--ui DIR            ui directory (default <repo>/ui)
--windows-out DIR   window output dir (default <ui>/windows)
--liga-header PATH  generated header (default client/src/html/engine/material_icons_liga.h)
--force             regenerate .ttf files even if present
--no-fonts          skip font conversion and the liga table
--no-windows        skip the window split
--no-editor         skip the map editor page
--no-social         skip the social hub page
--no-viewer         skip the model viewer page
--no-assets         skip the menu/HUD images (menu-assets.txt)
```
