# tools/ui-import

One-shot (but idempotent) importer that turns the Krunker UI sources into the static
inputs the native HTML engine consumes. The Makefile target `make ui-import` runs:

```sh
python3 -I tools/ui-import/import.py --ui ui
```

`--ref v7` (the default) imports Krunker v7.1.4 (`/home/user/jails/kru/src/Krunker_v7.1.4/public`,
`--public` overrides); `--ref v3` the former Krunker v3.1.1 import (the sections below marked
v3.1.1). Outputs are committed to the tree; re-running only rewrites files whose content changed.

## Krunker v7.1.4 (`--ref v7`)

| Step | Module | Output |
|---|---|---|
| menu page | `index_page.py` | `ui/index.html` from `public/index.html` (every `<script>`, comments, og/PWA links removed; `?build=`/`?v=` stripped; `https://assets.krunker.io/` and root-relative URLs made relative; external `<a href>` -> `href="#" data-ext-href`; `<canvas id="canvas">`, `css/krunknative.css` + `css/hud.css`, `js/kn-shim.js` + `js/loader.js` added) and its stylesheets (`main.css`, `bundled/bundledStyles.min.css`, `main_custom.css`, `flag.css`, `material-icons*.css`, `material-symbols-outlined.css`, `google-play.css`, plus `google-secular-one.css`) with the same URL rewriting, nothing else changed |
| fonts | `import.py` | every font file the stylesheets name is copied; each `.woff2` gets a `.ttf` next to it (material icon fonts named after their family: `material-icons.ttf`, `material-icons-outlined.ttf`; other faces after the woff2), and the ligature tables `client/src/html/engine/material_icons_liga.h` + `material_icons_outlined_liga.h` |
| windows | `windows_v7.py` | `ui/windows/` from `krunker-windows-v7.json.gz` (below): `<n>.html` = showWindow(n), `settings-{basic,advanced}-<t>.html`, `controls-<t>.html`, `windows.json` (header, sticky, class, inline style per window), `versions.txt` (= `public/docs/versions.txt`, the Changelog) |
| hub / viewer | `social_page.py`, `viewer_page.py` | `ui/social.html` (+ `social.css`, `social_custom.css`), `ui/viewer.html` (+ `dat-gui.css`); `--viewer-store` also regenerates `client/src/viewer/viewer_store_table.c` |
| editor | `editor_page.py` | only with `--editor-page`: v7's `editor.html` + editor CSS + CodeMirror CSS (`ui/libs/codemirror/`). Off by default while `client/src/editor/` drives the v3.1.1 editor page |
| images | `menu_assets.py` | every local file the pages, stylesheets and window templates reference (`collect_page_refs`), plus `menu-assets-v7.txt` (what C generates; `dest = v3:src` / `pattern = v3:` take files the v7.1.4 reconstruction lacks from v3.1.1). References v7 itself 404s on are listed, grouped by directory |

### Window captures (`capture_v7_windows.js`)

`node tools/ui-import/capture_v7_windows.js [out.json.gz]` loads v7's own client (`public/index.html`
with `js/index.js`) in headless Chromium against a read-only static serve of v7's `public/`
(`v7_page.js`; v7's npm server is never run: it writes into the reference tree). With no
matchmaker the page starts its offline sandbox; the capture sets a returning guest
(`krunker_haslogin`), undoes the sandbox's menu chrome (`CONNECTED_MENU`: `fullMenu()` plus the
elements `f2064` hides), then calls `showWindow(n, true)` for every `windows[]` entry and records
`#windowHeader`, `#menuWindow`'s class, inline style and markup, every settings tab of both views
(`windows[0].settingType` / `tabIndex`) and the five Change Controls tabs. The result is
committed as `krunker-windows-v7.json.gz` (gzipped JSON) so `make ui-import` needs no browser.
Needs `chromium-browser` (`KN_CHROME=`) and `net-bridge/node_modules` (`ws`, `tools/kref/cdp.js`).

`windows_v7.py` keeps the captured markup unchanged except: `?build=` stripped, the cosmetic
pickers' `#skinList` cut to their first 4 cards (templates; C lists the items), the host window's
Advanced tab bodies over 32 KB emptied (per-weapon/class matrices), and the hidden per-weapon
clones of the settings rows (`<key>_<weapon>_div`) dropped (no per-weapon settings storage yet).

## Krunker v10, the live krunker.io (`--ref v10`)

Writes a separate tree, `ui/v10/` (`ui/` stays v7.1.4). Provenance per file: `ui/README.md`.

```sh
# 1. mirror the live site (pages, stylesheets, fonts, every image they and the client script name)
KN_FETCH_PROXY=http://127.0.0.1:8888 python3 -I tools/ui-import/fetch_live_site.py --stage ~/.cache/kn-v10/live/site \
    --client-js ~/Downloads/v10.0.2.js --manifest tools/ui-import/menu-assets-v7.txt
# 2. the page tree (the client script is parsed by v10_bundle.js, never run)
python3 -I tools/ui-import/import.py --ui ui --ref v10 [--public <stage>/public] [--client-js v10.js]
```

| Step | Module | Output |
|---|---|---|
| site mirror | `fetch_live_site.py` | `<stage>/public/` (krunker.io + assets.krunker.io in one tree, each path asked of the host the reference names and then the other) + `<stage>/site.lock` (url, path, status, sha256, size, UTC date; resumable). Same politeness as `fetch_live_assets.py` (`KN_FETCH_JOBS` <= 4, delay, retries with backoff). `.js` is never downloaded |
| Svelte menu | `v10_bundle.js` | `mounts.json` (the HTML each boot `mount(Component, {target})` produces in the initial state), `svelte.css` (all `{hash, code}` component stylesheets), `i18n-en.json`, `report.txt`. acorn AST plus a whitelist interpreter over a toy DOM: Svelte runtime helpers are recognised by shape (template, sibling, if/each blocks, set_text, set_attribute, ...), unknown runtime calls are skipped, there is no host access and no `eval`. acorn: `net-bridge/node_modules` (`KN_NODE_MODULES=` from a worktree) |
| pages | `v10_page.py` | `ui/v10/{index,social}.html` (v7 rules + the mounts filled in + `css/svelte.css` linked), their stylesheets, our glue files, `windows/versions.txt` |
| windows | `windows_v10.py` | `ui/v10/windows/` from `krunker-windows-v10.json.gz` (the capture below): the layout and rules of `windows_v7.py`; windows 5 and 14 from v7's capture (`"from": "v7.1.4"`), the loadout windows' `#classPreviewCanvas` dropped (module docstring) |
| fonts, files | `import.py` | every font (`.ttf` next to each `.woff2`) and every file the pages, stylesheets and `menu-assets-v7.txt` name, from the mirror (`ui/` for what the hosts 404 on); `.webp.png` sidecars |

`capture_v10_live.js <stage>` is the rendered-DOM capture, which RUNS the live client. Run it
only with the user's approval. It uses sandboxed headless Chromium with a temporary profile,
`KN_FETCH_PROXY`, an allow-list of krunker.io / *.krunker.io / font+cdnjs hosts (GET/HEAD
only), no WebSocket and no sendBeacon. Blocked requests go to `blocked.tsv`. Output:
`<stage>/v10-capture/` (`windows.json` = every `showWindow(n)` plus the settings and Change
Controls tabs). Offline mode, used when krunker.io refuses the machine (Cloudflare 403, as on
2026-10-13): `KN_V10_PUBLIC=<stage>/public` answers every krunker.io request from the mirror
(`KN_V10_CLIENT_JS`, default `~/.cache/kn-v10/v10.js`, for `static/index-*.js`; `KN_V10_LIBS`,
default v7.1.4's `public/libs`, for the libs the mirror never fetched; an inert FRVR SDK
stand-in), with a dead proxy so nothing leaves the machine. Then:

```sh
KN_NODE_MODULES=<checkout>/net-bridge/node_modules KN_V10_PUBLIC=~/.cache/kn-v10/live/site/public \
    node tools/ui-import/capture_v10_live.js <stage>
python3 -I tools/ui-import/windows_v10.py pack <stage>/v10-capture/windows.json   # -> krunker-windows-v10.json.gz
python3 -I tools/ui-import/import.py --ui ui --ref v10                             # -> ui/v10/windows/
```

v10 content for `assets/` is pinned in `live_assets_v10.lock`
(`fetch_live_assets.py install --lock tools/ui-import/live_assets_v10.lock`): root `assets` for
new files, root `assets-v10` (= `assets/v10/<path>`) for v10 versions of files v7.1.4 has with
other content.

## Krunker v1.5.6 menu (`--ref v156`)

`make ui-import-v156` (`import.py --ui ui --ref v156`) writes `ui/v156/`, the menu page of
Settings > General "Game version" = v1.5.6 (menu only; v1.5.6 gameplay is not ported). Provenance
per file: `ui/v156/README.md`.

| Step | Module | Output |
|---|---|---|
| page | `v156_page.py` | `ui/v156/index.html` from v1.5.6's `public/index.html` (scripts, comments, ad / reCAPTCHA tags, favicon + description metas removed; `?build=`/`&v=` stripped; `/` and `./` URLs relative; external links neutralised; our canvas / css / js hooks as in `ui/index.html`, plus `css/kn-v156.css` and the `#popupHolder > #confPop` host `ui_confirm()` draws into), `css/main.css` + `css/material_icons.css` (URLs rewritten), `css/kn-v156.css` (v3.1.1's popup rules, `V3_POPUP_RULES`), our glue files from `ui/`, `windows/versions.txt` |
| files | `v156_page.py` | every local file the page and its stylesheets reference (`collect_page_refs`), from the v1.5.6 tree; what v1.5.6 itself 404s on is listed |
| fonts | `import.py` | `material-icons.ttf` next to `micon.woff2` (its ligatures equal v7's: the headers are not regenerated) |

## Krunker v3.1.1 (`--ref v3`)

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

### Krunker v7.1.4 content

`game_assets.py --v7` (`make assets-import-v7`; `make assets-check-v7` is the `--check` drift
mode) copies v7.1.4's `public/{models,textures,sound,videos,user-assets,scares}` into `assets/`
and writes `assets/skins.json` (the v7 store table: `v7_skins.js` unpacks `public/skins.jspck`
with v7's own `src/shim/jspck.js` and merges `store.js` templates) and `assets/previews.json`.
It refuses a skin table that would move an existing store index (inventory `skin_id`).
It also decodes every real WebP it copied (the player card banners, `textures/playercards/`)
into a `<name>.webp.png` sidecar (Pillow, first frame): the native HTML engine has no WebP
decoder and reads the sidecar for a `.webp` URL; the web build's browser decodes the `.webp`.
`gen_cosmetics.py` then regenerates `client/src/cosmetics_table.c` from `assets/skins.json`
(`--check`: exit 1 when out of date), and `gen_viewer_store.js` the client's full store table
(below). `v7_store.js` evaluates v7's own `store.js` / `itemTypes.js` / `config.js` (read-only)
for both: `assets/skins.json` is `store.skins` after store.js loaded (templates merged and its
noSpin / noSale / limited / limT fixups applied, `thumbnail` dropped), identical to the
reference's `skins.json`. Provenance and sizes: `assets/README.md`. Page images
stay under `ui/img` (menu import above); `assets/` is game content only.

### Krunker live asset hosts (files the v7.1.4 tree lacks)

`fetch_live_assets.py` (`make assets-import-live`, `make assets-check-live`) installs the files
pinned in `live_assets.lock` (root `assets` -> `assets/`, root `ui` -> `ui/img`; path, source URL,
sha256, size) that are missing from the tree, downloading them from `assets.krunker.io` /
`user-assets.krunker.io` (`KN_FETCH_PROXY`, `KN_FETCH_UA`, `KN_FETCH_JOBS` <= 4, `KN_FETCH_DELAY`)
or taking them from a `--stage` tree, and never overwrites a file. Its `fetch` mode downloads an
inventory TSV (category, path) into a staging tree, validates each file (Pillow decode, ffprobe,
OBJ/glTF/GLB/zip structure, no HTML/XML error pages, `.gltf` external URIs followed), records
url / status / sha256 / size per file and writes the lock from the valid 200s. The inventory
(every path v7.1.4's data tables, code literals and the 68 catalogue maps reference, plus
v7's own client and `client_bin` run on each map with request / open-failure logging) is
regenerated outside the repo. After `install`, deploy `--tags web_assets` (kn_web syncs
`assets/` to the web host, kn_updates re-indexes it as the launcher's content index).

## Model viewer page

`viewer_page.py` (step of `import.py`, skip with `--no-viewer`) writes
`ui/viewer.html` (Krunker's `public/viewer.html`, scripts removed) and
`ui/css/dat-gui.css` (dat.gui's injected stylesheet), and runs
`node gen_viewer_store.js [assets/skins.json] [out.c] [Krunker_v7.1.4 root]` (a `.map` argument, as
viewer_page.py still passes, is ignored), which regenerates `client/src/viewer/viewer_store_table.c`:
every v7.1.4 `store.skins` entry (6448, by index), rarities (incl. NFT), the 17 item types,
previews, skin colours, `currentSeason` 7, and the viewer's dropdown option lists computed like v7
`src/viewer.js` (f460 / f459 / melee, "[Free]" labels). `GEN_CHECK=1` only reports drift.

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
--ref v7|v3|v10|v156 Krunker reference (default v7; v10 = the live site into <ui>/v10/,
                    v156 = Krunker v1.5.6's menu page into <ui>/v156/)
--client-js FILE     v10: the live client script (parsed, never run)
--public DIR        the reference's public/ tree (default per --ref)
--viewer-store      also regenerate client/src/viewer/viewer_store_table.c (v7)
--editor-page       v7: also import editor.html + its CSS
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
