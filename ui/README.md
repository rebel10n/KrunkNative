# ui/ — the Krunker v10.0.3 menu as HTML/CSS

This is Krunker v10.0.3's menu page, the only page Krunker Box Office has (doc/v10/target.md). It was
imported from the v10.0.2 mirror (krunker.io, 2026-10-13); the 10.0.3 page has the same shell, Svelte
mounts, stylesheets and images, so only the English table, the buy popup and `windows/versions.txt`
changed (doc/v10/ui-1003-diff.md). A full `make ui-import` on the 10.0.3 bundle needs its deobfuscation
pass first (doc/v10/ui-1003-diff.md, "What differs"). It is rendered by the in-house
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
| `css/fonts/kn-symbols-serif.ttf`, `kn-symbols-sans.ttf`, `kn-symbols-emoji.ttf` (with `kn-symbols.ttf`) | Ours: the native engine's bundled symbol fallbacks, in this order (`client/src/html/engine/text.c` text_add_bundled_symbols). v10's page sets GameFont on every element (main.css `*`), and for a code point GameFont lacks Chrome tries its default standard font (Times New Roman, Liberation Serif on the reference machine) and then fontconfig's per-character fallback; each subset holds only the UI's code points that Chrome 152 draws from its source font there (read with CDP `CSS.getPlatformFontsForNode`, 2026-10-15). `kn-symbols-serif.ttf`: Liberation Serif Regular 2.1.5, U+00B7 U+00D7 U+2013 U+2014 U+2026 U+2190-2193 U+221E U+25BC (the ranked sort arrows, the Turf Wars KD ∞, the back arrow, dashes, ellipsis), renamed "KN Symbols Serif" for the license's Reserved Font Name (OFL 1.1, `css/fonts/kn-symbols-serif-OFL.txt`; sha256 28ed602485863bbbea12273f8e479cdad5fcd8108bb42b11494db67df6561aec). `kn-symbols-sans.ttf`: Adwaita Sans 50.0, its static opsz 14 / wght 400 instance, U+25B6 U+25C0 U+2605 U+2713 (the Players card ★, the Turf Wars rule ✓ and day arrows; OFL 1.1, `css/fonts/kn-symbols-sans-OFL.txt`; sha256 b8f7fa1ab1ce14935cc89a65a69b65981e8aadc552db380f43f610b7e99b4aef). Both made with fontTools (`fontTools.subset`, `varLib.instancer`) from Fedora 44's font packages. `kn-symbols-emoji.ttf`: monochrome Noto Emoji (`NotoEmoji-Regular.ttf`, OFL 1.1, `css/fonts/kn-symbols-emoji-OFL.txt`), U+2620 U+2694 U+269C U+1F3C6: the Raids, Clans and Ranked cards, which Chrome draws as these single-colour text glyphs (U+2694 U+2620 U+269C have text presentation); U+1F3C6 has emoji presentation, so where the UI writes 🏆 it is the colour image `img/kn/emoji/1f3c6.png` (below). |
| `css/*.css` (Krunker's), `css/fonts/*`, `img/`, `textures/`, the other images | Byte copies of the served files (URL rewriting only). Each `.woff2` also gets a `.ttf`, and each `.webp` a `.webp.png` sidecar (the engine decodes neither). Files the live hosts 404 on (the Play subsets, `favicon.png`, `pro_scope.png`, ...) are v7.1.4's copies (the retired page's). |
| `<video>` frame sidecars (`assets/videos/limited_shop_banner.webm.knv`, `assets/videos/tutorial/basics.mp4.knv`, `assets/scares/1.mp4.knv`; gitignored like the rest of `assets/`, shipped with the game content) | **Transcoded** from the v10 page's three videos (the Eterno Shop header, the Guide's Basics clip, `jumpScare(1)`; sha256-pinned in `tools/ui-import/live_assets_v10.lock`, fetched by `make assets-import-live`) by `make video-frames` (`python3 -I tools/ui-import/video_frames.py`: ffmpeg/ffprobe, baseline JPEG frames + a frame table, format in its docstring). The native engine has no video decoder: `client/src/html/engine/video.c` plays a `<video>` from `<file>.knv` next to its source, decoding each frame with the in-tree stb_image as playback reaches it; the web build plays the real file. Re-run after a source video changes (`--check` reports stale sidecars). |
| `windows/versions.txt` | The served `docs/versions.txt` (changelog). |
| `windows/` (`<n>.html`, `settings-{basic,advanced}-<t>.html`, `controls-<t>.html`, `windows.json`) | **Rendered**: what v10's own client writes into `#menuWindow` for `showWindow(1..54)`, every settings tab of both views and the five Change Controls tabs, captured by `capture_v10_live.js` (offline mode, below) and kept as `tools/ui-import/krunker-windows-v10.json.gz`; split by `tools/ui-import/windows_v10.py` with `windows_v7.py`'s rules (pickers cut to 4 cards, per-weapon setting clones and the host window's big Advanced bodies dropped, Download Client = ours). Window 14 (Store) is v7.1.4's capture (`"from": "v7.1.4"` in windows.json): offline, v10 cannot build the Store without its API. Window 5 (Account) is v10's own, empty: v10 shows a guest its login popup instead (autoWindowClose), and the signed-in Account window is generated (`account_ui.c`). Slots 11, 12, 22, 28, 29 are empty in v10. `2.html` (Servers) and `27.html` (Ranked) are empty on purpose too: v10's `gen()` returns `''` for them and fills the window at run time (`windows[1].genList()`, the ranked popup); KN builds them in `client/src/ui/servers_ui.c` (`servers_window_gen`) and `ranked_ui.c`. The `#classPreviewCanvas` v10 moves into the loadout windows is dropped from the templates (the page keeps the menu's one). |
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

What the native engine does not draw on this page (`aspect-ratio`, `-webkit-line-clamp` and `<video>`
are drawn: `<video>` from its frame sidecars, below). None of these is used by the window
templates, and a headless render of every `showWindow(1..54)`, settings tab and controls tab
on this page shows no blank window (`make smoke-ui` renders a subset); the stylesheet rules
that use them degrade as follows: `backdrop-filter` (frosted panels of Svelte popups: Turf Wars
territory panel, shop header, premium cards, notifications) is ignored, so those panels show
their plain translucent background; `mask-*` (`.menuItemIconMask` icons) is ignored, so a masked icon paints as a `currentColor`
square; `color-mix()` (`.chalRewTile`, `.titleRow`, `.titleSlotTag`, `.leaderTitle` gradients /
border) is not parsed, so those declarations are dropped and the tiles lose that tint.
Images the templates name that neither the mirror nor the retired v7.1.4 page had (broken images, as on v10
without its CDN): `img/mouse/10002.png` (controls), `img/backgrounds/gamecats/c_0.png` /
`c_9.png` (host / server type), `textures/previews/weapons/weapon_21.png`,
and `textures/previews/cosmetics/12_default.png`. Fetched since (pinned in `tools/ui-import/live_assets_v10.lock`):
`css/svelte.css`'s raffle backgrounds `img/lottery_bg*.webp` (from the page's own origin, krunker.io/img/) and the
editor's `img/templates/` and `img/editor-models/` thumbnails (assets.krunker.io; it has no `antenna_0*` / `event_0*`).

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
- `img/kn/emoji/{1f512,1f50d,2728,274c,2694-fe0f,1f6e1-fe0f,2705,1f46a}.png` — the text emoji v10
  writes in its Svelte popups: the ranked texts (Level Required / Login Required lock U+1F512, the
  Searching magnifier U+1F50D, Match found sparkles U+2728, Internal Error cross U+274C, Ready /
  in-game swords U+2694 U+FE0F; `client/src/ui/ranked_ui.c` emoji()) and the k-ID dialog's benefit
  icons (shield U+1F6E1 U+FE0F, check mark U+2705, the lock again, family U+1F46A; v10:117081,
  `client/src/ui/v10_misc_popups.c`). The native engine has no colour emoji font, so the text
  glyphs drew as missing-glyph boxes. v10 draws the same glyphs as text in Chrome through the
  system Noto Color Emoji font, so these are that font's artwork rasterised once: the glyph at
  160 px (line-height 1) in headless Chrome 152 `--screenshot` with a transparent background, in
  a window tall enough for the whole glyph, then its ink box centred on a 256 px square. Noto
  Color Emoji, `/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf` (sha256
  94c6c870925aaafd3fdda338fc4b2e4dfb74260c9aefa87d4aa8a145a993a5a2), OFL 1.1; 2026-10-14, and
  2026-10-15 for the k-ID glyphs plus a redo of the lock, sparkles, cross and swords, whose first
  rasters were cut off at the bottom (a 200 px tall capture viewport; the magnifier was whole
  and the redo reproduces it byte for byte). PNG sha256:
  1f512 bd595cff2222940d0012179c0f704664a24aa08d5ff202a4b281a7c16e942fdc, 1f50d
  7d32c8f2dcaa60336f16d0d366dc7a76fe1979d9934ce9119f6e55ef516f32bf, 2728
  d3ef762dbced2aedb1142d45dec9cdc5e3cff080f261dbd87f2eb2923f08f772, 274c
  f0bb857d8d185d9af4a1afedcd38edb5d62d7eefb98f69a16cc1cc08406e813d, 2694-fe0f
  b5b2f5aa527458c44cedfa81f5182f5d2969f5007bccecf356a3537fcc934eee, 1f6e1-fe0f
  ef5bc3b79cf333fdbd5d3c64251dbda78aecfd3ea287d51c9676928a2f35075b, 2705
  25a1e68416fb386be591027d5d40dc93da6bdaa572ba26e3f6e7a53312641d0f, 1f46a
  084efd3294649c86235975e8d2851fd297b92b099bdcb6fc7bf4bd725fed598e. Not in
  `tools/ui-import/live_assets_v10.lock`: v10 has no origin file for these glyphs (its page does
  not ship emoji images), so the lock's fetch-and-verify model does not apply.
  The Turf Wars emoji follow the same rule (`client/src/ui/v10_competitive.c` v10_emoji(), the
  table shared with the ranked texts): `img/kn/emoji/{1f3ae,1f3af,1f465,1f4b0,1f4ca,1f947,1f948,
  1f949,26a0-fe0f}.png` (the Turf Wars how-to's Earn Kills / Earn Rewards / Pro Tips / note icons,
  the Top Grossing Clans card and the My Clan medals), the same Noto Color Emoji rasterisation.
  PNG sha256: 1f3ae a361b0ef9e16c1e0333d1c40ba65ac915876db3a7526a6828fdb820838e3c384,
  1f3af 12f5f61e1eaf4a2277c58538b93fe41d61626cf61f5e66d30c4c992fad8d9153, 1f465
  1fdaf693e8cb4c4d4939b13a5d85c3639a006927981052ad1b3eead2ee093542, 1f4b0
  ed7694f0bc65fdfe7a6817a8da90dd27e7d6d74b15f03dacbd939694f8b49f89, 1f4ca
  716d53d1fad73518994ed9f0c26eaea9d0771ed53cb59f04e9bf80ba4e08a31b, 1f947
  1636fecc7b8ba07e9450cf1c4d95221b4cc06223b64f80e7ba97ec5540fe1152, 1f948
  d265fd094633c5b7f9b90c5c91aca76089b95e573f8669e08c07d8f56e82bafd, 1f949
  d65ece522506ee9dc1731bb83dbf2c1fc8351ed09a707e803e53ded4b4db76c2, 26a0-fe0f
  9ea8993b0702ddc5611a0bb2aa4bc14d4510f43682d729955ec3c8b5e0379816.
  The 2694 sword of the Turf Wars tips is `2694-fe0f.png` above.
  `img/kn/emoji/{1f3c6,2b50}.png` (2026-10-15): the 🏆 of the Leaderboards KrunkCup card and the Turf
  Wars Top Performing Clans / Control Territories cards, and the ⭐ of the Turf Wars Top Performing
  Players card, which Chrome draws with Noto Color Emoji (`CSS.getPlatformFontsForNode`); the same
  raster (the glyph at 160 px, line-height 1, in a 512x640 transparent `--screenshot` window, its ink
  box centred on a 256 px square, the whole glyph: ink 178x174 and 177x169 px). PNG sha256: 1f3c6
  743e7ff82707a2aa4991de96f38be87a00596b4421725334517165ea1e2b0580, 2b50
  e746c5c47fdb2f3ecae480c8b8442955f6a6f89ed923ea8c255993b243b439bc. The Clans / Raids / Ranked cards' U+2694 U+2620 U+269C and the Players card's U+2605
  are text glyphs in Chrome, not colour emoji: the bundled `kn-symbols-emoji.ttf` and
  `kn-symbols-sans.ttf` (table above).
  Placement (`css/kn-v10-menu.css` .kn-emoji): the image takes the glyph's own box, Noto Color Emoji's
  1.245em advance and 1em of line, with the ink centre 0.35em above the baseline as in Chrome (the
  ink centres of all 19 rasters sit within 0.03em of each other, except 👥 0.1em lower); the ranked
  overlay's block emoji sits on GameFont's line instead. Checked against headless Chrome drawing v10's
  markup at the same boxes: the cards' icons within 1 px, the overlay ❌ and the footer ✨ within 1 px.
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

## Map thumbnails (`img/maps/`)

Every map card the menu draws (Games window, Host Game, Ranked carousel and match history, challenges,
the server list and the community list) takes its picture from the catalogue entry's `thumb`
(`shared/src/map_catalog.c`, `map_catalog_thumb`) or, for the official maps, `img/maps/map_<mapIndex>.png`.
A map with no file shows `img/noimg.png`. `shared/tests/map_catalog_test.c` checks that every file below
loads (a PNG with its IHDR) and that the ranked pool's pictures exist; a catalogue map with no picture must be
listed as missing in `thumb_missing_on_purpose()` (the "missing" rows). Community pictures are pinned in
`tools/ui-import/live_assets.lock` (root `ui`) and fetched with `make assets-import-live` (through
`KN_FETCH_PROXY` when the asset hosts need it).

Source column: `v10` = `https://assets.krunker.io/img/maps/map_<mapIndex>.png`, the picture v10.0.2's Host Game and
Games cards load for an official map (`/img/maps/map_" + mapIndex + ".png"` in the v10 client), pinned in
`tools/ui-import/live_assets_v10.lock`. `v7` = Krunker v7.1.4 `public/img/maps/` (byte-identical copy), `v3.1.1` = Krunker v3.1.1
`public/img/maps/map_<n>.png`, where `n` is v3.1.1's MAPS index (`main/maps.js`: the same order as the `311_`
entries), `user-assets` = `https://user-assets.krunker.io/m<map id>/thumb.png`, the picture Krunker's own menu
shows for a community map; its map id comes from `https://gapi.svc.krunker.io/maps/by-name/<name>` (fetched
2026-10-15 through the proxy). The creators gapi reports: Bhop_VIP eskrr, ClashRoyale Trivm2002, Krunk_Plaza
Sidney, Slide_Neon_Stage R75, SniperRoyale2 JavieBhop, Verb_Arena Iniquity_Games, zombie_high IamFiercer,
slide_moonlight Corrade, FortniteTiltedT DjDankMeister, Test v e c t o r (2018).

| key | name | file under `ui/` | source | status |
|---|---|---|---|---|
| `burg_new` (v7 0) | Burg | `img/maps/map_0.png` | v7 `public/img/maps/map_0.png` | present |
| `littletown` (v7 1) | Littletown | `img/maps/map_1.png` | v7 `public/img/maps/map_1.png` | present |
| `sandstorm_v3` (v7 2) | Sandstorm | `img/maps/map_2.png` | v7 `public/img/maps/map_2.png` | present |
| `subzero_new` (v7 3) | Subzero | `img/maps/map_3.png` | v7 `public/img/maps/map_3.png` | present |
| `undergrowth` (v7 4) | Undergrowth | `img/maps/map_4.png` | v7 `public/img/maps/map_4.png` | present |
| `shipment` (v7 5) | Shipment | `img/maps/map_5.png` | v7 `public/img/maps/map_5.png` | present |
| `freight` (v7 6) | Freight | `img/maps/map_6.png` | v7 `public/img/maps/map_6.png` | present |
| `lostworld_s6` (v7 7) | Lostworld | `img/maps/map_7.png` | v7 `public/img/maps/map_7.png` | present |
| `citadel` (v7 8) | Citadel | `img/maps/map_8.png` | v7 `public/img/maps/map_8.png` | present |
| `oasis_v3` (v7 9) | Oasis | `img/maps/map_9.png` | v7 `public/img/maps/map_9.png` | present |
| `kanji` (v7 10) | Kanji | `img/maps/map_10.png` | v7 `public/img/maps/map_10.png` | present |
| `industry` (v7 11) | Industry | `img/maps/map_11.png` | v7 `public/img/maps/map_11.png` | present |
| `lumber` (v7 12) | Lumber | `img/maps/map_12.png` | v7 `public/img/maps/map_12.png` | present |
| `evacuation` (v7 13) | Evacuation | `img/maps/map_13.png` | v7 `public/img/maps/map_13.png` | present |
| `site` (v7 14) | Site | `img/maps/map_14.png` | v7 `public/img/maps/map_14.png` | present |
| `skytemple_v2` (v7 15) | SkyTemple | `img/maps/map_15.png` | v7 `public/img/maps/map_15.png` | present |
| `lagoon` (v7 16) | Lagoon | `img/maps/map_16.png` | v7 `public/img/maps/map_16.png` | present |
| `bureau` (v7 17) | Bureau | `img/maps/map_17.png` | v7 `public/img/maps/map_17.png` | present |
| `shipwrecked` (v7 18) | Tortuga | `img/maps/map_18.png` | v7 `public/img/maps/map_18.png` | present |
| `tropicano` (v7 19) | Tropicano | `img/maps/map_19.png` | v7 `public/img/maps/map_19.png` | present |
| `krunk_plaza_v2` (v7 20) | Krunk_Plaza | `img/maps/map_20.png` | v7 `public/img/maps/map_20.png` | present |
| `arena` (v7 21) | Arena | `img/maps/map_21.png` | v7 `public/img/maps/map_21.png` | present |
| `habitat_v2` (v7 22) | Habitat | `img/maps/map_22.png` | v7 `public/img/maps/map_22.png` | present |
| `atomic` (v7 23) | Atomic | `img/maps/map_23.png` | v7 `public/img/maps/map_23.png` | present |
| `burg_2018` (v7 24) | Old_Burg | `img/maps/map_24.png` | v7 `public/img/maps/map_24.png` | present |
| `throwback_2018` (v7 25) | Throwback | `img/maps/map_25.png` | v7 `public/img/maps/map_25.png` | present |
| `stockade_2018` (v7 26) | Stockade | `img/maps/map_26.png` | v7 `public/img/maps/map_26.png` | present |
| `facility` (v7 27) | Facility | `img/maps/map_27.png` | v7 `public/img/maps/map_27.png` | present |
| `clockwork` (v7 28) | Clockwork | `img/maps/map_28.png` | v7 `public/img/maps/map_28.png` | present |
| `laboratory` (v7 29) | Laboratory | `img/maps/map_29.png` | v7 `public/img/maps/map_29.png` | present |
| `shipyard` (v7 30) | Shipyard | `img/maps/map_30.png` | v7 `public/img/maps/map_30.png` | present |
| `sanctum` (v7 31) | Soul Sanctum | `img/maps/map_31.png` | v7 `public/img/maps/map_31.png` | present |
| `bazaar` (v10 32) | Bazaar | `img/maps/map_32.png` | v10 `assets.krunker.io/img/maps/map_32.png` (live, 2026-10-19 through the proxy) | present |
| `erupt` (v10 33) | Erupt | `img/maps/map_33.png` | v10 `assets.krunker.io/img/maps/map_33.png` | present |
| `hq` (v10 34) | HQ | `img/maps/map_34.png` | v10 `assets.krunker.io/img/maps/map_34.png` | present |
| `khepri` (v10 35) | Khepri | `img/maps/map_35.png` | v10 `assets.krunker.io/img/maps/map_35.png` | present |
| `lush` (v10 36) | Lush | `img/maps/map_36.png` | v10 `assets.krunker.io/img/maps/map_36.png` | present |
| `vivo` (v10 37) | Vivo | `img/maps/map_37.png` | v10 `assets.krunker.io/img/maps/map_37.png` | present |
| `slide_moonlight_v10` (v10 38) | Slide Moonlight | `img/maps/community/slide_moonlight.png` | the community `slide_moonlight` picture: the same map (1068 objects in both files) | present |
| `eterno_simulator` (v10 39) | Eterno Simulator | `img/maps/map_39.png` | the ranked pool's picture (see below) | present |
| `stalk_factory` (v10 40) | Stalk Factory | `img/maps/map_40.png` | v10 `assets.krunker.io/img/maps/map_40.png` | present |
| `eterno_jump_v10` (v10 41) | eterno_jump | `img/maps/map_41.png` | v10 `assets.krunker.io/img/maps/map_41.png` (the community `eterno_jump` has none) | present |
| `frontier` (v10 42) | Frontier | `img/maps/map_42.png` | the ranked pool's picture (see below) | present |
| `bastion` (v10 43) | Bastion | `img/maps/map_43.png` | v10 `assets.krunker.io/img/maps/map_43.png` | present |
| `piazza` (v10 44) | Piazza | `img/maps/map_44.png` | v10 `assets.krunker.io/img/maps/map_44.png` | present |
| `barnyard` (v10 45) | Barnyard | `img/maps/map_45.png` | v10 `assets.krunker.io/img/maps/map_45.png` | present |
| `311_burg` | Burg (3.1.1) | `img/maps/v311/map_0.png` | v3.1.1 `public/img/maps/map_0.png` | present |
| `311_littletown` | Littletown (3.1.1) | `img/maps/v311/map_1.png` | v3.1.1 `public/img/maps/map_1.png` | present |
| `311_sandstorm` | Sandstorm (3.1.1) | `img/maps/v311/map_2.png` | v3.1.1 `public/img/maps/map_2.png` | present |
| `311_subzero` | Subzero (3.1.1) | `img/maps/v311/map_3.png` | v3.1.1 `public/img/maps/map_3.png` | present |
| `311_undergrowth` | Undergrowth (3.1.1) | `img/maps/v311/map_4.png` | v3.1.1 `public/img/maps/map_4.png` | present |
| `311_shipyard` | Shipyard (3.1.1) | `img/maps/v311/map_5.png` | v3.1.1 `public/img/maps/map_5.png` | present |
| `311_freight` | Freight (3.1.1) | `img/maps/v311/map_6.png` | v3.1.1 `public/img/maps/map_6.png` | present |
| `311_lostworld` | Lostworld (3.1.1) | `img/maps/v311/map_7.png` | v3.1.1 `public/img/maps/map_7.png` | present |
| `311_citadel` | Citadel (3.1.1) | `img/maps/v311/map_8.png` | v3.1.1 `public/img/maps/map_8.png` | present |
| `311_oasis` | Oasis (3.1.1) | `img/maps/v311/map_9.png` | v3.1.1 `public/img/maps/map_9.png` | present |
| `311_kanji` | Kanji (3.1.1) | `img/maps/v311/map_10.png` | v3.1.1 `public/img/maps/map_10.png` | present |
| `311_newtown` | Newtown (3.1.1) | `img/maps/v311/map_11.png` | v3.1.1 `public/img/maps/map_11.png` | present |
| `311_industry` | Industry (3.1.1) | `img/maps/v311/map_12.png` | v3.1.1 `public/img/maps/map_12.png` | present |
| `311_soul_sanctum` | Soul Sanctum (3.1.1) | `img/maps/v311/map_13.png` | v3.1.1 `public/img/maps/map_13.png` | present |
| `311_evacuation` | Evacuation (3.1.1) | `img/maps/v311/map_14.png` | v3.1.1 `public/img/maps/map_14.png` | present |
| `152_burg` | Burg (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `152_freight` | Freight (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `152_kanji` | Kanji (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `152_littletown` | Littletown (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `152_lostworld` | Lostworld (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `152_sandstorm` | Sandstorm (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `152_shipyard` | Shipyard (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `152_subzero` | Subzero (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `152_undergrowth` | Undergrowth (1.5.2) | none | none found: no v1.5.x map picture in the references, gapi has the current map only | missing |
| `Bhop_VIP` | Bhop_VIP | `img/maps/community/Bhop_VIP.png` | user-assets `m127262/thumb.png` | present |
| `ClashRoyale` | ClashRoyale | `img/maps/community/ClashRoyale.png` | user-assets `m4774/thumb.png` | present |
| `ESP_Parkour` | ESP_Parkour | none | no gapi map by this name, no user-assets id | missing |
| `FortniteTiltedT` | FortniteTiltedT | none | user-assets `m4955/thumb.png` is a 404 | missing |
| `Krunk_Plaza` | Krunk_Plaza | `img/maps/community/Krunk_Plaza.png` | user-assets `m136558/thumb.png` | present |
| `Slide_Neon_Stage` | Slide_Neon_Stage | `img/maps/community/Slide_Neon_Stage.png` | user-assets `m144394/thumb.png` | present |
| `SniperRoyale2` | SniperRoyale2 | `img/maps/community/SniperRoyale2.png` | user-assets `m17737/thumb.png` | present |
| `Test` | Test | none | user-assets `m51/thumb.png` is a 404 | missing |
| `Verb_Arena` | Verb_Arena | `img/maps/community/Verb_Arena.png` | user-assets `m104011/thumb.png` | present |
| `bot_ftue_v2` | Mini Industry | none | no gapi map by this name, no user-assets id | missing |
| `eterno_jump` | eterno_jump | none | no gapi map by this name, no user-assets id | missing |
| `slide_moonlight` | slide_moonlight | `img/maps/community/slide_moonlight.png` | user-assets `m17738/thumb.png` | present |
| `zombie_high` | Zombie_High | `img/maps/community/zombie_high.png` | user-assets `m148853/thumb.png` | present |

Ranked pool (`shared/src/ranked.c`): `map_2`, `map_4`, `map_11`, `map_14`, `map_17` are v7 (present); `map_39`
(Eterno Simulator) is v7's file, `map_42` (Frontier) is the live asset host's `img/maps/map_42.png` (both present).

Missing: 25 of the 83 catalogue maps (the 9 `152_` maps, 5 community maps and 11 v10 additions above). Before the
v10 additions, 14 of the 69 were missing (the 9 `152_` maps and 5 community maps). The `152_` maps are v3.1.1's old_* copies of the v1.5.2 maps; the
references hold no picture of them and only the current maps of those names. `Krunk_Plaza`'s picture is byte-identical to the v7
official `Krunk_Plaza` (map 20). `Test` (creator v e c t o r on gapi) and `FortniteTiltedT` have no thumb on the host.


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
