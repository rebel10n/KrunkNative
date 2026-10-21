# tools/ui-import

One-shot (but idempotent) importer that turns the Krunker UI sources into the static
inputs the native HTML engine consumes. The Makefile target `make ui-import` runs:

```sh
python3 -I tools/ui-import/import.py --ui ui
```

It imports Krunker v10.0.2 (the bundle ui/ was built from; the target is Krunker v10.0.3, doc/v10/target.md), from the live site mirror
into `ui/` (until 2026-10-14 the v10 page was the tree `ui/v10/` and `ui/` was imported from
Krunker v7.1.4: git history has that importer's steps). The page has been edited by hand since
(the v10 parity work), so a re-import overwrites those edits: run it into a scratch copy
(`--ui <dir>`) and merge. Outputs are committed to the tree; re-running only rewrites files whose
content changed.

`windows_v7.py`, `index_page.py`, `social_page.py` and `menu-assets-v7.txt` stay as helpers of the
v10 steps (the v7 capture `krunker-windows-v7.json.gz` supplies windows 5 and 14).

## Krunker v10, the live krunker.io

Writes `ui/`. Provenance per file: `ui/README.md`.

```sh
# 1. mirror the live site (pages, stylesheets, fonts, every image they and the client script name)
KN_FETCH_PROXY=http://127.0.0.1:8888 python3 -I tools/ui-import/fetch_live_site.py --stage ~/.cache/kn-v10/live/site \
    --client-js ~/Downloads/v10.0.2.js --manifest tools/ui-import/menu-assets-v7.txt
# 2. the page tree (the client script is parsed by v10_bundle.js, never run)
python3 -I tools/ui-import/import.py --ui ui [--public <stage>/public] [--client-js v10.js]
```

| Step | Module | Output |
|---|---|---|
| site mirror | `fetch_live_site.py` | `<stage>/public/` (krunker.io + assets.krunker.io in one tree, each path asked of the host the reference names and then the other) + `<stage>/site.lock` (url, path, status, sha256, size, UTC date; resumable). Same politeness as `fetch_live_assets.py` (`KN_FETCH_JOBS` <= 4, delay, retries with backoff). `.js` is never downloaded |
| Svelte menu | `v10_bundle.js` | `mounts.json` (the HTML each boot `mount(Component, {target})` produces in the initial state), `svelte.css` (all `{hash, code}` component stylesheets), `i18n-en.json`, `report.txt`. acorn AST plus a whitelist interpreter over a toy DOM: Svelte runtime helpers are recognised by shape (template, sibling, if/each blocks, set_text, set_attribute, ...), unknown runtime calls are skipped, there is no host access and no `eval`. acorn: `net-bridge/node_modules` (`KN_NODE_MODULES=` from a worktree) |
| page | `v10_page.py` | `ui/index.html` (v7 rules + the mounts filled in + `css/svelte.css` linked), its stylesheets, `windows/versions.txt` |
| windows | `windows_v10.py` | `ui/windows/` from `krunker-windows-v10.json.gz` (the capture below): the layout and rules of `windows_v7.py`; windows 5 and 14 from v7's capture (`"from": "v7.1.4"`), the loadout windows' `#classPreviewCanvas` dropped (module docstring) |
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
python3 -I tools/ui-import/import.py --ui ui                                       # -> ui/windows/
```

v10 content for `assets/` is pinned in `live_assets_v10.lock`
(`fetch_live_assets.py install --lock tools/ui-import/live_assets_v10.lock`): root `assets` for
new files, root `assets-v10` (= `assets/v10/<path>`) for v10 versions of files v7.1.4 has with
other content.

## What it produces

| Output | Source | Notes |
|---|---|---|
| `ui/css/fonts/material-icons.ttf` | every `.woff2` referenced by an `@font-face` in `ui/css/*.css` that exists (today only Material Icons; the Secular One files are missing and skipped) | file name = slug of the rule's `font-family`; an existing loadable `.ttf` is kept unless `--force` |
| `client/src/html/engine/material_icons_liga.h` | GSUB ligature lookups of the Material Icons woff2 (Google serves the font with `rlig`) | `MaterialIconLiga material_icons_liga[MATERIAL_ICONS_LIGA_COUNT]` = `{name, codepoint, glyph_index}` sorted by name in byte order (+ `MATERIAL_ICONS_LIGA_MAX_NAME`); the format of `gen_liga.py` / what `engine/text.c` consumes |

`gen_liga.py` is the standalone ligature/TTF generator (`--font --header [--ttf]`);
`import.py` loads it when present so both emit byte-identical headers, and
falls back to an equivalent built-in writer otherwise. Saved TTFs use
`SOURCE_DATE_EPOCH=0` (unless set) so re-conversions are reproducible.

## Menu and HUD images

`menu_assets.py` (step of `import.py`, skip with `--no-assets`) copies every file listed
in `menu-assets-v7.txt` from the v7.1.4 `public/` tree (`v3:` lines: the v3.1.1 tree) into `ui/` at the same path,
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

## Social hub and editor pages (Krunker v10)

`v10_hub_editor.py` (step of `import.py`; `--no-social` / `--no-editor`
skip a page; also runs alone: `python3 -I tools/ui-import/v10_hub_editor.py --ui ui [--public <mirror>]`)
writes `ui/social.html` and `ui/editor.html` from the live krunker.io pages of the
`fetch_live_site.py` mirror (`~/.cache/kn-v10/live/site/public`), their stylesheets
(`social.css`, `social_custom.css`, `editor.css`, `editor_dark.css`, `editor_custom.css`,
`pickr.css`, the shared Vite `static/index-<hash>.css` as `static/index.css`) and the images they
reference that `ui/` lacks. KrunkNative additions are re-applied on every run (module docstring):
the web glue, the editor's `css/editor-kn.css` / `editor-script.css` / `editor-ai.css` and
`#knAiOption`, the hub's Download badge, v7.1.4's `#twoFAPop` with `css/kn-twofa.css` (generated
from v7's editor.css), and `css/hub-svelte.css` (the view stack / popup / LoginModal component
blocks of `v10_bundle.js`'s `svelte.css`). The v10 bundles (`static/social-*.js`,
`static/editor-*.js`) are never fetched by the mirror nor run; the C ports read them through
webcrack (`doc/v10/editor-social.md`). `social_page.py` / `editor_page.py` (v7.1.4 / v3.1.1 pages) are only used when the v7 step is skipped.

## Dependencies

`fonttools` and `brotli` must be importable under `python3 -I` (isolated mode
ignores user site-packages), i.e. installed system-wide:
`dnf install python3-fonttools python3-brotli` or
`sudo pip install fonttools brotli`.

## Options

```
--ui DIR            ui directory (default <repo>/ui)
--public DIR        the live site mirror's public/ tree (default ~/.cache/kn-v10/live/site/public)
--client-js FILE    the live client script (parsed by v10_bundle.js, never run)
--viewer-store      also regenerate client/src/viewer/viewer_store_table.c
--liga-header PATH  generated header (default client/src/html/engine/material_icons_liga.h)
--force             regenerate .ttf files even if present
--no-fonts          skip font conversion and the liga table
--no-windows        skip the window split
--no-editor         skip the map editor page (v10_hub_editor.py)
--no-social         skip the social hub page (v10_hub_editor.py)
--no-viewer         skip the model viewer page (Krunker v7.1.4's)
--no-assets         skip the page / C UI images (menu-assets-v7.txt)
```
