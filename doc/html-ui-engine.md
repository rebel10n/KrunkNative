# KrunkNative HTML UI engine

KrunkNative shows Krunker v3.1.1's own menu and HUD: the page is `ui/index.html`
(Krunker's markup + `css/main.css`, trimmed, see [ui/README.md](/ui/README.md)), and
the behaviour Krunker's game JavaScript gave it is ported to C. **No JavaScript runs in
the native client**: the in-house HTML/CSS engine ignores `<script>`, and the C UI
logic drives the page through a small DOM-operations API. The web build reuses the
same C UI logic against the browser's DOM, with a thin JS shim as the only glue.

```
            client/src/ui/*.c  (menu_ui, ui_handlers, ui_settings, ui_windows, hud_ui)
                         |  html_dom.h (DOM ops, events, timers, storage)
        +----------------+-----------------+--------------------------+
        | native (default)                 | web (Emscripten)         | KRUNKNATIVE_HTML_ENGINE=OFF
        | client/src/html/engine/          | client/src/html/web_dom.c| client/src/html/stub.c
        |   dom_ops.c + engine.c           |   + ui/js/kn-shim.js     |   no-op DOM ops,
        |   Lexbor + FreeType + GL         |   browser renders page   |   html_ui_available() == 0
        +----------------------------------+--------------------------+
                         |  html_ui.h (init/update/render/input) via html_ui.c
                       client/src/main.c
```

## Layers and files

| Layer | Files | Role |
|---|---|---|
| Client API | `client/include/html_ui.h`, `client/src/html/html_ui.c` | `html_ui_init/fini/resize/update/render`, input forwarding (`html_ui_mouse_*`, `html_ui_key`, `html_ui_char`, `html_ui_scroll`), menu flag (`html_ui_set_menu_open`/`html_ui_is_menu_open`), `html_ui_is_ready`, `html_ui_available`, pages (`html_ui_load_page`, `html_ui_current_page`, `html_ui_page_serial`; see [Pages](#pages)) |
| Backend contract | `client/src/html/html_ui_backend.h` | `html_ui_backend_*` implemented by exactly one backend; the backend calls `html_ui_page_ready()` once the DOM can be queried |
| DOM-ops API | `client/include/html_dom.h` | what UI logic may do to the page (below) |
| DOM-ops core | `client/src/html/html_dom_core.c` | handler table, listeners, dispatch by name, timers, native storage; shared by every backend |
| Native backend | `client/src/html/engine/` ([README](/client/src/html/engine/README.md)) | Lexbor DOM, CSS cascade, layout, paint, GL compositing, events, form widgets; `dom_ops.c` implements `html_dom_*` |
| Web backend | `client/src/html/web_dom.c`, `ui/js/kn-shim.js`, `ui/js/loader.js` | `EM_JS` DOM ops over the browser DOM; page events reach C through `kn_ui_event` |
| Stub | `client/src/html/stub.c` | minimal builds without HTML UI |
| UI logic | `client/include/ui_logic.h`, `client/src/ui/{menu_ui,ui_handlers,ui_settings,ui_windows,hud_ui}.c` | menu state machine, inline-handler table, settings store, window templates (`ui/windows/`), in-game HUD |
| Tools | `client/src/html/ui_test.c` (`make ui-test`), `client/src/ui/ui_scenario.c` + `tools/uidiff` (`make uidiff`), `tools/ui-import` (`make ui-import`) | engine probes/benchmarks, pixel diffs against `doc/ref/krunker-v3.1.1/`, regenerating `ui/` from the Krunker reference |

## Frame contract (`html_ui.h`)

`main.c` calls, on the main (GL/GLFW) thread:

1. `html_ui_init(user, ui_dir, fb_w, fb_h)` once (`ui_dir` is `../ui/` for dev builds,
   `./ui/` for release, `KRUNKNATIVE_UI_DIR` overrides). Returns 0 when no backend is
   compiled in; the client then keeps its built-in menu/HUD (`hud.c`, `overlay.c`).
2. `client_menu_bind()` (UI logic) installs the handler table and listeners.
3. Per frame: `html_ui_update()` (page events -> handlers, resize observers, timers),
   `client_menu_tick()` (UI logic: connection state, menu/HUD DOM writes), the 3D scene,
   the class preview, then `html_ui_render()` (native: composite the page last,
   premultiplied; leaves depth test off and blending on).
4. Input: while `html_ui_is_menu_open()` the client releases the pointer and forwards
   mouse/keyboard to the page in framebuffer pixels; in game the page receives no input
   and only shows the HUD. ESC opens the menu natively; with the menu open the page's
   `keydown` listener decides (blur a focused field, close a window, close the menu).
5. `html_ui_fini()` at shutdown.

`html_ui_is_ready()` turns 1 once the backend has a DOM (native: after the first
layout; web: at init). The UI logic runs its one-time page setup (`menu_dom_init`)
the first tick `#uiBase` exists.

## DOM-ops contract (`html_dom.h`)

Element handles are `HtmlElem` (`uint32_t`, 0 = none): natively an index into the
engine's node table, on the web the `data-kn-id` the shim assigns. Handles of detached
elements stay valid. All geometry is in CSS px from the viewport's top-left.

- **Lookup / tree**: `html_dom_root/body/by_id/query/query_all/parent/first_child/next/closest/child_count/tag`.
  `by_id` returns the first match in document order (Krunker's markup has duplicate ids).
- **Mutation**: `html_dom_create`, `html_dom_create_text`, `html_dom_append`,
  `html_dom_insert_html` (insertAdjacentHTML), `html_dom_remove`, `html_dom_set_html`
  (innerHTML), `html_dom_set_text`/`get_text` (textContent).
- **Attributes / classes / style**: `set_attr/remove_attr/get_attr` (-1 when absent),
  `set_class(el, cls, on)` (classList.toggle), `has_class`, `set_display`,
  `set_style(el, prop, value)` (setProperty, custom properties included; "" removes),
  `get_style` (inline style only, -1 when unset).
- **Form state**: `set_value/get_value`, `set_checked/get_checked`, `focus`, `blur`,
  `scroll_to_bottom`.
- **Geometry** (valid after layout; 0 while `display:none`): `get_rect`
  (getBoundingClientRect), `offset_width/height`, `viewport(w, h, dpr)`,
  `observe_resize(el, fn, user)` (ResizeObserver equivalent, coalesced per frame),
  `element_at(x, y)` (elementFromPoint, pointer-events aware) and `active_element()`
  (document.activeElement; 0 for none/body), which the map editor uses to tell clicks on
  its 3D view from clicks on the page and to know when the user is typing.

Buffer-returning getters copy into `buf`/`n` and return the length needed.

### Events: page -> C

Every inline `on*` attribute in the markup is a call by name, resolved against one
handler table (`html_ui_set_handlers`, installed by `ui_handlers_install()`):

- `onclick="showWindow(1)"` dispatches handler `showWindow` with args `[NUM 1]`;
  `windows[0].changeTab(2)` becomes `windows.0.changeTab`; a leading `return` is dropped.
- Arguments are `HtmlArg`: numbers, strings, booleans, `this`, `this.value`,
  `this.checked`, `event`. The `HtmlEvent` carries the type (`click`, `mousedown`,
  `input`, `change`, `keydown`, `keypress`, `mouseenter`, `mouseover`, `focus`, `blur`,
  `resize`), the element the handler is attached to, its value/checked state, the key
  (GLFW code natively, DOM keyCode on the web: test with `html_key_is_enter/escape`),
  the mouse button and position.
- A handler returns 0 to prevent the default action (like `return false`).
- Names the table does not know go to the fallback handler; `html_ui_current_handler()`
  tells it which name is being dispatched. Krunker functions the real page never
  defined are no-ops, as a ReferenceError would be in the browser.

The handful of listeners Krunker's JS added with `addEventListener` are registered with
`html_ui_set_listener(elem_id, type, fn)` (`elem_id` NULL = window/document):
`#instructionHolder mousedown` (click to play / close the open window),
`#windowHolder mousedown` (backdrop closes the window), document `click` (never
navigate: links are blocked and logged), window `keydown` (Escape/Enter handling),
window `resize` (`menu_scale_base`).

Every dispatch is logged to stderr as `[ui] handler name(args) type target=N` or
`[ui] listener ...` (hover events are not logged), which is what the smoke tests grep.

### Timers and storage

- `html_timer_add(ms, fn, user)` / `html_timer_cancel(id)` replace `setTimeout`; the
  backend calls `html_timers_tick(now_ms)` from `html_ui_update()`, firing timers in due
  order (callbacks may add/cancel timers; new ones never fire in the same tick).
- `html_storage_get/set(key, value)` replace `localStorage`: natively one file per key,
  `$XDG_CONFIG_HOME/krunknative/<key>.json` (else `~/.config/krunknative/`, Windows
  `%USERPROFILE%`); on the web the browser's `localStorage`. Keys in use: `knSettings`
  (settings JSON, Krunker's own setting names), `classindex`, `knName`, `consent`; the map editor adds `kro_editor` (its settings, Krunker's key), `knEditorMap` (autosave of the edited map) and `custMap`/`custToLoad` (Test/Host Map exports, Krunker's keys).

## Pages

Besides the game page (`ui/index.html`) the client shows Krunker's other pages, each a
separate document of `ui/` with its own C logic: the social hub (`ui/social.html`,
`client/src/ui/social_ui.c`), the map editor (`ui/editor.html`, `client/src/editor/`) and
the model viewer (`ui/viewer.html`, `client/src/viewer/`). The client's *app mode*
(`client/include/app_mode.h`, `client/src/app_mode.c`) says which one is shown:

- **Entry points**: `client_bin --hub|--editor|--viewer [query]`; Krunker's links and
  buttons (`window.open('/social.html')`, `/editor.html`, `/viewer.html`,
  `/social.html?p=profile&q=x`, the hub's KRUNKER logo `openLink('/')`) go through
  `ui_open_external()` -> `app_route_url_target()`; UI logic calls `app_set_mode()`.
- **Native**: every page opens in its own window, like the v3.1.1 Electron client
  (`electron . --social/--editor/--viewer`): `client/src/app_window.c` starts a
  `client_bin --windowed [--offline] --hub|--editor|--viewer <query>` process per page kind
  (Electron's window sizes: hub 1200x600, editor 1200x800, viewer 800x600; same working
  directory and environment, so the same config dir and server settings) and leaves the
  game window alone. Opening a page whose window exists reloads it with the new query and
  raises it; a page's links to the game raise the game window (or open one, `--game`, when
  the page was started on its own). The windows talk over a pipe pair (fds 3/4,
  `KRUNKNATIVE_APP_IPC`); a window closes when the window that opened it closes. A process
  per window instead of a second GLFW window: the client's state (the Client, the HTML
  engine's documents and handler contexts, GL caches, audio) is process-wide. In a page
  process `html_ui_load_page(page)` shows that document (a fresh engine instance, `g_page`
  in `engine/engine.c`; the game page's instance `g_base` stays loaded but is never shown);
  `client_tick()` hands the frame to `app_mode_frame()`, which calls the page's `AppPage`
  hooks (`enter`, `tick`, `leave`, input, `resize`) with app_mode.c's own GLFW callbacks
  (pointer free, ESC does not open the game menu). Editor and viewer windows do not connect
  to the game server.
- **Web**: every page is its own URL, like on Krunker: `dist/web/index.html`,
  `social.html`, `editor.html`, `viewer.html` all load `js/kn-shim.js`, `js/loader.js` and
  the same `client.js`, which boots in the mode its file name says (`location.pathname`;
  the page query is `location.search`). Switching pages is a browser navigation (a new
  tab where Krunker opens one); `?offline=1` / `?ws=` carry over. `html_ui_load_page()`
  only accepts the document the browser shows.
- **Handler contexts**: each page has its own handler table, listeners and timers
  (`html_dom_core.c`; context 0 = the game page, context 1 = the other page, cleared for
  every fresh document). A page installs its table with `html_ui_set_handlers()` in its
  `enter` hook, when `html_ui_is_ready()` is 1 again; `html_ui_page_serial()` changes with
  every document. A handler name `window.foo` falls back to `foo` when the table has no
  `window.foo`. On the web `kn-shim.js` installs forwarders for every call it finds in the
  page's `on*` attributes (and in markup C inserts later), not only its static list.
- **Social data**: the hub's requests travel over the game connection as JSON frames
  (`NET_PACKET_SOCIAL_REQUEST` / `_REPLY`, `shared/include/social.h`), answered by
  `server_bin` or the in-process offline game the way the v3.1.1 offline server answers
  them (no accounts: guest views, the server's map list as the map catalogue).
- **uidiff**: scenario `state.page` / `pageQuery` / `pageActions` (tools/uidiff/README.md).

## Backends

### Native: the in-house engine (default)

`KRUNKNATIVE_HTML_ENGINE=ON` (the default; `make`) compiles `client/src/html/engine/*.c`,
`client/lib/lexbor` and links FreeType into `client_bin` with
`-DKRUNKNATIVE_HTML_ENGINE=1`. It is pure C11 and builds on Linux, Windows (MinGW) and
macOS. Inline handlers are parsed from the `on*` attributes and dispatched with DOM
bubbling; `<script>` elements are ignored. `html_ui_update()` works at most 120 times a
second, animation ticks are capped at the display refresh rate, and a static page costs
one display-list submit per frame (restyle/layout/paint run only when something is
dirty). Module map and internals: [client/src/html/engine/README.md](/client/src/html/engine/README.md);
architecture and phases: [doc/html-engine-plan.md](/doc/html-engine-plan.md).

### Web: browser DOM + `web_dom.c`

The Emscripten build always uses `client/src/html/web_dom.c` (+ `html_ui.c`,
`html_dom_core.c`); the engine is not compiled for the web. `ui/index.html` loads
exactly two scripts:

- `ui/js/kn-shim.js`: element ids (`knId`/`knEl`, `data-kn-id`), `ResizeObserver`
  forwarding (`kn_ui_resize_observed`), `localStorage` access, and `knActivate()`, which
  `html_ui_backend_init()` calls: it replaces every handler global the markup calls
  (`setSetting`, `showWindow`, `windows[n].*`, `SOUND.play`, ...) with a forwarder that
  `ccall`s `kn_ui_event("<type>|<name>", argsJson, target, value, checked, key, button, x, y)`,
  and installs the listeners above as `listener:<elemId>:<type>` events. No UI logic
  lives in JS.
- `ui/js/loader.js`: loads `client.js`/`client.wasm` when the page is served over HTTP
  (no-op from `file://`).

The link exports `_kn_ui_event` and `_kn_ui_resize_observed`, and preloads
`ui/windows` into MEMFS (`/ui/windows`, `client.data`) for the window templates.
`?kn_selftest=1` runs a DOM-ops self test in the page and prints PASS/FAIL lines to the
console (`window.__kn_selftest`).

### Stub: no HTML UI

`KRUNKNATIVE_HTML_ENGINE=OFF` (`make HTML_ENGINE=OFF`) builds `stub.c` instead of the
engine: `html_ui_init()` fails, `html_ui_available()` is 0 and every DOM op is a no-op
returning "no element". The UI logic is still compiled (it keeps the connection state
in `client_menu_tick()`), and the client uses its built-in menu text and HUD.

## Debugging knobs

| Variable | Effect |
|---|---|
| `KRUNKNATIVE_UI_DIR=<dir>` | load the page from another directory instead of `ui/` |
| `KRUNKNATIVE_HTML_PROFILE=1` | engine: per-stage timings (restyle/layout/paint/submit) every ~2 s |
| `KRUNKNATIVE_HTML_VERBOSE=1` | engine: log loads, failures and first-layout stats |
| `KRUNKNATIVE_HTML_DUMP_DL=1` | engine: print the display list once per paint build |
| `KRUNKNATIVE_HTML_IMAGE_LOG=1\|<file>` | image: one `image-miss` line (key, url as written, referencing document, file path, reason) per image that fails to resolve, read or decode, on stderr (`1`) or appended to `<file>` |
| `KRUNKNATIVE_HTML_DISPLAY_HZ=<hz>` | engine: override the detected refresh rate that caps animation ticks |
| `KRUNKNATIVE_HTML_HINTING=none\|slight\|full` | text: FreeType hinting (default none) |
| `KRUNKNATIVE_HTML_TEXT_GAMMA=<gamma>:<contrast>` | text: gamma/contrast of the glyph coverage LUT (default 2.2:0.5) |
| `KRUNKNATIVE_HTML_OBLIQUE_SKEW=<k>` | text: synthetic oblique shear |
| `KRUNKNATIVE_HTML_FAKE_BOLD_ADVANCE=0\|1` | text: add the synthetic-bold strength to the advance |
| `KRUNKNATIVE_HTML_FALLBACK_FONT=<path>[:<path>...]` | text: fallback faces for missing glyphs |
| `KRUNKNATIVE_AUTOPLAY=1` (web `?kn_autoplay=1`) | enter the game as soon as page and map are ready (headless HUD screenshots) |
| `KRUNKNATIVE_HUD_DEMO=1` (web `?kn_hud_demo=1`) | loop kill-feed rows and hit markers |
| `KRUNKNATIVE_PREVIEW_TEST=1`, `KRUNKNATIVE_PREVIEW_DUMP=<file.ppm>`, `KRUNKNATIVE_PREVIEW_WORLD=1` | class-preview debugging (`client/src/ui/class_preview.c`) |

## Checks

- `make ui-test ARGS=lexbor-probe` (and `ARGS=help` for render-page, bench, dump-boxes, ...).
- `make uidiff [SCENARIOS=...]`: renders `tools/uidiff/scenarios/*.json` with
  `client_bin --render-scenario` under Xvfb/llvmpipe and pixel-diffs each region
  against the Krunker references ([tools/uidiff/README.md](/tools/uidiff/README.md)).
- `make smoke-native`: native client offline under Xvfb; passes when the C UI logic set
  up the page (`[ui] setting ...` lines), screenshot in `/tmp/smoke-native.png`.
- `make smoke-web`: web build in headless Chromium, screenshot in `/tmp/smoke-web.png`.
