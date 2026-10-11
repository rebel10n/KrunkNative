# KrunkNative

Krunker.io - except it's awesome. A rewrite of the game in native C using minimal libraries, focused on freedom and performance.

![Demo](/doc/demo.png)

# Building

The client, server and tests build with CMake, driven by the Makefile (`make help`). Everything is C11 and vendored (GLFW, GLAD, FreeType, Lexbor, cJSON, replxx), including the in-house HTML/CSS engine that renders the menu and HUD; platform-specific instructions below.

## Windows

CMake + w64devkit/MinGW should work out of the box

## Linux

Please install the following Debian packages to build GLFW:

- libwayland-dev
- wayland-protocols
- pkg-config
- libx11-dev
- libxcursor-dev
- libxinerama-dev
- libxrandr-dev
- libxi-dev
- libgl1-mesa-dev
- libglu1-mesa-dev
- libxkbcommon-dev

Full command: `apt install -y libwayland-dev wayland-protocols pkg-config libx11-dev libxcursor-dev libxinerama-dev libxrandr-dev libxi-dev libgl1-mesa-dev libglu1-mesa-dev libxkbcommon-dev`

# Running

If you wish to run KrunkNative, you must first add game assets, please read [the assets readme](/assets/README.md) for more information. The client looks for the `assets` folder in the same directory in release mode, or in the parent directory in development mode (when the binary is inside the `cmake-build-release` or `cmake-build-debug` folder).

Everything goes through the Makefile (`make help` lists all targets; CMake does
the actual compiling, presets are in `CMakePresets.json`). By default the client
connects to a native TCP server on `127.0.0.1:21015`. Start the bridge or server
first, then launch the client:

```sh
make            # debug client, server and tests (HTML engine on)
make run        # debug client
make server     # debug server
make release    # release binaries (make run-release / server-release)
make test       # shared movement/collision test
```

To play offline (single player: the client runs the game itself, no server), pass `--offline`:

```sh
make run ARGS=--offline MAP=kanji
```

## Packet Logging

Set `KRUNKNATIVE_PACKET_LOG=1` before launching the client to print decoded, color-coded client packets. Incoming packets are marked `<=` in orange, and outgoing packets are marked `=>` in green.

Linux:

```sh
KRUNKNATIVE_PACKET_LOG=1 ./cmake-build-debug/client_bin
```

Windows PowerShell:

```powershell
$env:KRUNKNATIVE_PACKET_LOG = "1"; .\cmake-build-debug\client.exe
```

Leave `KRUNKNATIVE_PACKET_LOG` unset, or set it to `0`, to disable packet logging.

## Other debug switches

| Variable | Effect |
|---|---|
| `KRUNKNATIVE_UI_DIR=<dir>` | load the HTML menu from another directory instead of `ui/` |
| `KRUNKNATIVE_HTML_PROFILE=1` | print per-stage HTML engine timings (restyle/layout/paint/submit) every ~2 s |
| `KRUNKNATIVE_HTML_VERBOSE=1` | log HTML engine loads, failures and first-layout stats |
| `KRUNKNATIVE_HTML_DUMP_DL=1` | print the engine's display list once per paint build |
| `KRUNKNATIVE_HTML_DISPLAY_HZ=<hz>` | override the refresh rate that caps the page's animation ticks |
| `KRUNKNATIVE_HTML_HINTING=none\|slight\|full` | FreeType hinting of page text (default none) |
| `KRUNKNATIVE_HTML_TEXT_GAMMA=<gamma>:<contrast>` | text gamma/contrast (default 2.2:0.5) |
| `KRUNKNATIVE_AUTOPLAY=1` | enter the game as soon as the page and map are ready |
| `KRUNKNATIVE_PREVIEW_TEST=1` | force a fixed class-preview rectangle while the menu is open |
| `KRUNKNATIVE_PREVIEW_DUMP=<file.ppm>` | dump the raw framebuffer after the class-preview draw |
| `KRUNKNATIVE_PREVIEW_WORLD=1` | also place a third-person copy of the preview model in the world scene |

More engine knobs (text rendering, HUD demo) are listed in
[doc/html-ui-engine.md](/doc/html-ui-engine.md#debugging-knobs).

# Web build (Emscripten)

The client also compiles to WebAssembly/WebGL2 and runs in a browser.

Prerequisites: a working [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
(CI pins Emscripten 6.0.12; run `emsdk install latest && emsdk activate latest`
and source `emsdk_env.sh` so `emcmake` is on your `PATH`), CMake and Node.

```sh
make web                   # EMSDK=/path/to/emsdk if it is not ~/emsdk or on PATH
```

This configures `cmake-build-web` with the `web` CMake preset (Emscripten
toolchain, Release) and assembles `dist/web/`: the `ui/` page tree,
`client.js`, `client.wasm`, `client.data` (the preloaded `ui/windows`
templates) and an `assets/` symlink. In the browser the page is the UI; the
same C UI logic as natively drives its DOM through `client/src/html/web_dom.c`,
and `ui/js/kn-shim.js` + `ui/js/loader.js` are the only scripts (web glue, no
UI logic). For a quick local look
`make web-serve` serves that directory with Python's `http.server`; to host it
together with the WebSocket bridge to a native server:

```sh
make bridge                # cd net-bridge && npm run web
```

Then open <http://localhost:8080>. The bridge serves `dist/web` over HTTP
(override the directory with `--serve-web <dir>` or `KRUNK_NATIVE_WEB_DIR`)
and upgrades WebSocket connections on `/native` into a raw TCP passthrough to
the native server on `127.0.0.1:21015` (`KRUNK_NATIVE_HOST` /
`KRUNK_NATIVE_PORT`). Every binary WebSocket frame is written to the TCP
socket as-is and every TCP chunk comes back as a binary frame, so the web
client speaks exactly the same framed protocol as the native one. Start
`make server` (or `make server-release`) first, or append `?offline=1`
to the page URL to play offline in the browser (the game runs in-process) with no
backend at all.

The bridge listens on `KRUNK_NATIVE_BRIDGE_HOST:KRUNK_NATIVE_BRIDGE_PORT`
(default `127.0.0.1:8080`). Set `KRUNK_NATIVE_WEB_COI=1` if the build uses
threads and needs `SharedArrayBuffer` (adds the COOP/COEP headers).

CI builds the web client on every push (`KrunkNative-web` artifact) and
publishes it to GitHub Pages on pushes to `main`. The published page carries
**no game assets** (they are ~1.5 GB and not in the repo), so models and
textures 404 there, and there is no `/native` bridge behind GitHub Pages;
only the `?offline=1` path is usable on the hosted page. It is meant as a
smoke test of the build, not as a playable deployment. Pages must be set to
"GitHub Actions" as the source in the repository settings for the deploy job
to succeed.

# HTML UI (in-house engine)

The menu and in-game HUD are Krunker v3.1.1's own HTML/CSS (`ui/index.html`),
rendered natively by KrunkNative's HTML/CSS engine: pure C11 on top of
[Lexbor](https://github.com/lexbor/lexbor) (HTML parsing, DOM, selectors) and
FreeType, drawn with OpenGL over the 3D scene (`client/src/html/engine/`). There
is no JavaScript in the native client: the engine ignores `<script>`, and the
behaviour Krunker's game JS gave the page is ported to C in `client/src/ui/`
(`menu_ui.c`, `ui_handlers.c`, `ui_settings.c`, `ui_windows.c`, `hud_ui.c`),
which drives the page through a small DOM-operations API
(`client/include/html_dom.h`). The contract, backends and debug knobs are
documented in [doc/html-ui-engine.md](/doc/html-ui-engine.md); the engine's
modules in [client/src/html/engine/README.md](/client/src/html/engine/README.md).

The engine is on by default. To build a minimal client without any HTML UI (the
built-in menu text and HUD are used instead):

```sh
make HTML_ENGINE=OFF       # or: cmake --preset debug -DKRUNKNATIVE_HTML_ENGINE=OFF
```

UI tooling:

```sh
make ui-test ARGS=lexbor-probe   # engine driver: probes, render-page, bench, dump-boxes (ARGS=help)
make uidiff                      # render tools/uidiff scenarios with client_bin and pixel-diff them
                                 # against the Krunker references (needs Xvfb + ImageMagick)
make ui-import                   # regenerate ui/ (fonts, windows, icon ligatures) from the Krunker reference
make smoke-native smoke-web      # headless screenshots of both clients in /tmp
```

# UI

`ui/` is a trimmed copy of the Krunker v3.1.1 menu (HTML, CSS, fonts and
images; no game JavaScript) plus the window templates in `ui/windows/` and
the web-only glue `js/kn-shim.js` and `js/loader.js`. It is ordinary source:
edit it directly. [ui/README.md](/ui/README.md) records what was imported and
how.

## Acknowledgements

- OpenGL - graphics backend
- GLFW - cross-platform OpenGL helper
- GLAD - OpenGL loader
- FreeType - font rasterizer
- Lexbor - HTML/CSS parser and DOM used by the HTML engine
- STB Image - single-header image loading library
- tinyobjloader-c - single-header OBJ loading library
- verstable - single-header hash table library
- cJSON - lightweight JSON parser
- pcg_basic - pseudorandom number generator

# FRVR Slander (we ❤️ Yendis)

![Brian](/doc/brianGOD.png)

Apparently we're less clever than FRVR, we could NEVER be creators...  
![You were never a creator](/doc/email.png)

We tried to negotiate with Brian to work for FRVR and give them every script/exploit we had for ~$10k (shared across ~10 people)  
![Negotiations](/doc/negotiations.png)  
![Negotiations](/doc/negotiations_brian.png)
