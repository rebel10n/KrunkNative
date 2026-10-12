# tools/uidiff

Per-region pixel diff of the native HTML/CSS UI against the Krunker v7.1.4
reference captures in `doc/ref/krunker-v7.1.4/` (made by `capture-v7.js`, see
[References](#references); the v3.1.1 captures stay in `doc/ref/krunker-v3.1.1/`). `make uidiff` builds `client_bin`
with the in-house engine (`HTML_ENGINE=ON`) and renders every scenario through the
real client and its C UI logic (see [Rendering with client_bin](#rendering-with-client_bin)):

```sh
make uidiff                                  # all scenarios
make uidiff SCENARIOS="menu-1280 win-classes" # a subset
```

`tools/uidiff/uidiff.sh <client_bin> [scenario-name...]` is the pipeline underneath;
without `UIDIFF_RENDER_CMD` it calls `<client_bin> --render-scenario <scenario> <out>`.

Requirements: bash, ImageMagick (`magick`, or `convert` + `compare`), python3, and
Mesa llvmpipe with either `xvfb-run` (Xvfb) or libEGL (`UIDIFF_HEADLESS=1`).

## Pipeline

For each `scenarios/<name>.json`:

1. `xvfb-run -a -s "-screen 0 <W>x<H>x24" env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe <client_bin> --render-scenario scenarios/<name>.json <out>/<name>.png`
   (headless: `KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE=<device> <client_bin> --render-scenario ...`, no xvfb-run)
2. every region of `regions/<name>.json` is cropped from the reference and the
   render and compared with `compare -metric AE -fuzz <fuzz>%`; the share of
   differing pixels (`DIFF%`) must be `<= max_pct`.
3. a table (scenario, region, diff %, max %, fuzz, PASS/FAIL/SKIP) is printed,
   `<out>/<name>.diff.png` (compare's visual with the region rectangles: green
   PASS, red FAIL, gray SKIP), `<out>/<name>.sbs.png` (reference | render) and
   `<out>/<name>.regions/{ref,render,diff}-<region>.png` are written.

Exit status 0 when every non-skipped region passes, 1 when any region fails or a
render is missing / has the wrong size, 2 on usage or tool errors.

Environment:

| Variable | Default | Meaning |
|---|---|---|
| `UIDIFF_OUT` | `cmake-build-debug/uidiff` | output directory |
| `UIDIFF_FUZZ_SCALE` | `1` | multiplies every region's fuzz (loosen globally while text rendering is tuned) |
| `UIDIFF_RENDER_CMD` | unset | replaces `<client_bin> --render-scenario` (and its xvfb-run); run as `$UIDIFF_RENDER_CMD <scenario.json> <out.png>` with `UIDIFF_REF`, `UIDIFF_W`, `UIDIFF_H`, `UIDIFF_SCENARIO` exported. The literal `cp` copies the reference as the render (pipeline self-test). |
| `UIDIFF_NO_XVFB` | `0` | `1` runs the renderer on the inherited `DISPLAY` (already inside an Xvfb; never the user's desktop) |
| `UIDIFF_HEADLESS` | `0` | `1` renders headless (no window, no X server) on Mesa llvmpipe, pixel-identical to Xvfb |
| `UIDIFF_EGL_DEVICE` | unset | `nvidia`/`mesa`/`<index>`: headless on that EGL device (implies `UIDIFF_HEADLESS=1`) |
| `UIDIFF_XVFB_ARGS` | `-a` | extra `xvfb-run` options |

`tools/uidiff/selftest.sh [scenario...]` validates the pipeline without the
engine: an identical render must pass and a 3 px shifted one (via
`stub-render.sh` + `UIDIFF_STUB_SHIFT`) must fail.

## Rendering with client_bin

```
client_bin --render-scenario <scenario.json> <out.png>
```

`make uidiff` exports `UIDIFF_RENDER_CMD="<repo>/cmake-build-debug/client_bin --render-scenario"`,
`LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe`, and runs the whole `uidiff.sh` inside one
`xvfb-run -s "-screen 0 1920x1080x24"` (`UIDIFF_XVFB_SCREEN`, `UIDIFF_XVFB_ARGS`;
`UIDIFF_NO_XVFB=1` uses the current display). Run it by hand from the repo root (or
from `cmake-build-debug`: paths resolve against `assets_path()`'s root):

```sh
xvfb-run -a -s "-screen 0 1920x1080x24" env LIBGL_ALWAYS_SOFTWARE=1 \
  ./cmake-build-debug/client_bin --render-scenario tools/uidiff/scenarios/menu-1280.json /tmp/menu.png
```

Headless (no window, no X server; `client/include/headless.h`): `make uidiff
UIDIFF_HEADLESS=1` renders on Mesa llvmpipe through EGL (pixel-identical to the Xvfb
path), `make uidiff UIDIFF_EGL_DEVICE=nvidia` on the NVIDIA GPU (driver-specific bugs:
the real stencil/varying behaviour of the user's RTX card, with nothing on screen). By hand:

```sh
env -u DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE=nvidia \
  ./cmake-build-debug/client_bin --render-scenario tools/uidiff/scenarios/menu-1280.json /tmp/menu.png
```

Implementation: `client/src/ui/ui_scenario.c` (+ hooks in `client/src/main.c`). The
client starts as usual with these differences:

- a hidden, non-resizable window of exactly `size` (`KRUNKNATIVE_SCENARIO_VISIBLE=1`
  shows it); the offline local server on the map named `state.mapName` (the
  `assets/maps/*.json` whose `"name"` matches, loaded like `client_bin <map.json>`, so
  the map list is that map + the rotation); without `mapName` the default rotation
  with a fixed RNG seed (repeatable map pick, the host/games windows list the
  rotation as in the references); `html_storage`
  (settings, class index, name) in a temporary `XDG_CONFIG_HOME`, removed on exit;
- the world is never drawn: after the frame's `glClear` the `background` is painted
  (flat colour, or the image stretched over the framebuffer). The first-person weapon
  and the class preview are drawn only with `state.preview: true`; without it the
  preview rectangle gets the preview's own 50 % backdrop over a flat background (an
  image background already holds the real preview pixels);
- the window's stencil buffer is filled with 0xFF before the UI draws: its contents are
  undefined on real drivers (non-zero on NVIDIA, 0 on llvmpipe), so a renderer that
  assumes a clean stencil fails here on llvmpipe too;
- a virtual 60 Hz clock drives the engine (`engine_set_clock_override`: animations,
  timers, caret) and the HUD (`hud_ui_set_clock`: hit marker, 20 Hz pacing). The
  first frame runs at clock 0, which is where the page's animations start; the capture
  happens at the first `k * 1.6 s + anim_time` after the state has settled (1.6 s is
  the cycle of every infinite menu animation: `grow` 0.8 s alternate, `scaleBob`
  0.4 s alternate), reached by a jump and `frames` frames ending exactly on it;
- the last frame is read back from the default framebuffer's back buffer
  (`glReadPixels`, flipped to top-left origin, alpha forced to 255) and written with
  `client/src/png_write.c` (shared with `ui_test`).

How `state` is applied (all through the real UI logic, after the page, the map and the
local server are ready):

| Key | C path |
|---|---|
| `name`, `classIndex` | `Client.menu_name` + `menu_update_class_box()`, `menu_set_class_index(i, 1)` |
| `region` | `menu_set_region_label()` (shown while connected; default `MENU_REGION_NAME`) |
| `inGame` | `client_set_menu_open(0)` + `client_enter_game()` (ENT -> SPAWN via the local server), then the HUD runs from `client_hud_html_tick()` |
| `hud` | `hud_ui_set_override()`: replaces the sampled values (weapons looked up by name; `scoped` shows Krunker's reticle only for scope weapons, other weapons just aim: crosshair hidden) |
| `scoreboard` | `hud_ui_set_override_scoreboard()`: rows `{rank, name, score, me?}`; `me` defaults to "only row" or name == `name` (`.newLeaderNameM`) |
| `chat` | `hud_ui_clear_chat()` + `hud_ui_add_chat()`: `name`+`msg` = player line, a non-white `color` = server line (type 2), white/none = plain game line (type 1) |
| `menu` | `client_set_menu_open()` (default `!inGame`) |
| `gameName` | `menu_set_game_name()`: `#mapInfo` text + `#infoHolder` shown |
| `window`, `settingsTab` | `menu_show_window(id, 1)`; `0` = `windows[0]` = settings (`showWindow(1)`), `"host"`/`"join"`; with `settingsTab` the page's own `windows.0.changeTab(N)` handler |
| `dead` + `killcard` | `hud_ui_show_death()` (Krunker `g1()` markup + `toggleGameUI(false)`) |
| `endscreen` | `hud_ui_show_end_screen()` (`showEndScreen(false)` + `#victorySub` + `#endTimer` "Next Round in ..."); `place` 1 = victory (optional `result` 1/0/-1). `text` is not markup: the `#1 VICTORY` title is drawn on Krunker's overlay canvas |
| `chatOpen`, `chatInput` | `html_dom_set_value()` + `html_dom_focus()` on `#chatInput` |
| `connected`, `ready` | `MenuUi.connected/ready` + `menu_update_top_bar()` + `menu_update_instructions()` (applied last) |
| `killfeed` | `hud_ui_add_killfeed_ex()` with the row's `age_ms` at the captured frame (fade/removal timers) |
| `hit` | `hud_ui_hit_elapsed()`: the hit happened `elapsed_ms` before the captured frame |
| `preview` | bool (extension): draw the first-person weapon and the class preview |
| `scrollbars` | bool (extension): paint scrollbars; default off = headless Chromium's `--hide-scrollbars`, which most references were captured with (`win-reticle` was not) |
| `modeName` | only "Free for All" exists (a warning otherwise) |
| `page`, `pageQuery`, `pageActions` | (extension) show another page instead of the game menu: `app_set_mode()` with `page` = `hub` / `editor` / `viewer` and the URL query (`"p=maps"`), then each `pageActions` entry is dispatched as an inline handler (`"openSettings"`, `"loginPopup"`); every other key is ignored. The `hub-*` scenarios (references `doc/ref/krunker-v7.1.4/hub/`) use it |

Debugging: `KRUNKNATIVE_SCENARIO_DUMP=<selector>` prints the captured frame's box tree
of the first match (`engine_dump_boxes`), e.g. to tell a layout bug from a state bug.

## CLI contract for the renderer

```
client_bin --render-scenario <scenario.json> <out.png>
```

- Paths inside the scenario (`ref`, `background`) are relative to the repository
  root; `uidiff.sh` runs from the root and passes absolute paths for the
  scenario and the output.
- Create an offscreen GL context / hidden GLFW window of exactly `size`
  (`[w, h]`, CSS px == device px, devicePixelRatio 1), load `ui/index.html`
  (same asset lookup as the client), apply `state`, run `frames` update+render
  iterations (so layout, resize observers and timers settle) with the animation
  clock of the captured frame at `state.anim_time` seconds, then read the
  framebuffer back and write an opaque `w x h` PNG, top-left origin.
- `background` is painted under the UI before compositing: `#rrggbb` or an image
  path (see [Backgrounds](#backgrounds)). Default: opaque black.
- Exit 0 and write the PNG on success; non-zero with a message on stderr
  otherwise (missing asset, GL failure, unknown state key is a warning only).

### Scenario JSON

```json
{
  "ref": "doc/ref/krunker-v7.1.4/menu/menu-1280.png",
  "size": [1280, 720],
  "background": "#d6c6c6",
  "state": { ... },
  "frames": 3,
  "note": "free text"
}
```

`state` keys (all optional; unknown keys are ignored with a warning):

| Key | Type | Meaning |
|---|---|---|
| `menu` | bool | menu open (`menu_apply_open`) |
| `window` | int, string or null | window to show: v7.1.4 `showWindow(N)` ids (0 = 1 settings, 2 servers, 3 Change Loadout, 4 mods, 5 account, 7 controls, 8 host, 9 spray, 10 custom games, 14 store, 24 join, 25 client, 27 ranked, 31 attachment, 32 reticle, 33 challenges, 35 changelog, 42 host type, 48 server finder), `"host"` = 8, `"join"` = 24, `null` = none |
| `settingsTab` | int | `windows[0].changeTab(N)` when `window` is 0 |
| `settingsView` | `"basic"` / `"advanced"` | `windows[0].settingType` (v7's default is basic) |
| `windowActions` | array of `[name, args...]` | inline-handler calls dispatched once the window is open, by the names v7's markup uses: `["windows.2.switchTab", 0]` (Change Loadout's Class tab), `["windows.6.switchTab", 2]` (a controls tab) |
| `connected`, `ready` | bool | net state for `#instructions` (CLICK TO PLAY when both true) |
| `inGame` | bool | HUD visible when `inGame && !menu`; with `menu` also true = ESC pause |
| `mapName`, `modeName`, `region`, `name`, `gameName`, `classIndex` | string / int | state glue (`#curGameInfo`, `#menuRegionLabel`, `#menuClassNameTag`, `#gameNameHolder`, class box) |
| `anim_time` | number | animation clock (s) of the captured frame (see [anim_time](#anim_time)) |
| `hud` | object | the `hud` payload of `ui/js/hud.js`: `health, maxHealth, healthSegments, ammo, ammoMax, ammoLimit, melee, reloading, reloadPercent, timerText, kills, deaths, score, weaponName, weaponIcon, classIndex, scoped, fps, aim, spread, scoreboard, ping, loadout[{name,icon,melee}], loadoutIndex` |
| `chat` | array of `{text, color}` | `#chatList` rows (server messages are `#fc03ec`); a row may carry `name`/`msg` for player lines |
| `chatOpen`, `chatInput` | bool, string | `#chatInput` focused with the given text |
| `killfeed` | array | `killfeed` payloads (`killer, victim, weapon, weaponIcon, killerIsYou, victimIsYou, headshot, age_ms`) |
| `scoreboard` | array of `{rank, name, score}` | `.newLeaderItem` rows shown while `hud.scoreboard` is true |
| `dead`, `killcard` | bool, `{killer, weapon, level, score, classIndex, stats}` | death screen (`#deathUIHolder`; `stats` = v7's `[timeAlive ms, shots, "acc%", kills, score]`), HUD holders hidden |
| `endscreen` | `{place, text, nextRound}` | end screen (`#endUI`), HUD hidden |
| `hit` | `{elapsed_ms, kill}` | hit marker `elapsed_ms` after the hit |

### Regions JSON

```json
{
  "scenario": "menu-1280",
  "defaults": { "fuzz": 6, "max_pct": 0.5 },
  "regions": [
    { "name": "logo", "rect": [400, 8, 480, 144], "kind": "image", "fuzz": 8, "max_pct": 1.5, "note": "..." },
    { "name": "class-preview", "rect": [1040, 288, 190, 96], "kind": "scene", "skip": true },
    { "name": "ammo", "rect": [1125, 650, 140, 40], "kind": "text", "fuzz": 12, "max_pct": 3.0,
      "mask": "colors", "mask_colors": ["#ffffff", "#ffd542"], "mask_color_fuzz": 2.5 },
    { "name": "window-header", "rect": [406, 177, 468, 70], "ref_offset": [0, -24] },
    { "name": "empty-top-left", "rect": [0, 0, 200, 100], "kind": "flat", "expect": "background" }
  ]
}
```

| Key | Meaning |
|---|---|
| `rect` | `[x, y, w, h]` in output pixels (render coordinates) |
| `kind` | documentation only: `text` 12 % / 3.0, `flat` 6 % / 0.5, `image` 8 % / 1.5, `scene` (skipped) |
| `fuzz` | per-pixel tolerance in percent (`compare -fuzz`; in mask modes: max channel difference) |
| `max_pct` | allowed share of differing pixels (in mask modes: of the mask's pixels) |
| `skip` | measured and printed, never fails; `note` says why |
| `mask` | `none` (default, every pixel), `colors` or `ink`: compare only "ink" pixels, the union of the reference's and the render's. `colors`: pixels within `mask_color_fuzz` % (default 12) of one of `mask_colors` (an element's opaque text/fill colours). `ink`: pixels differing from a `mask_kernel`² median of the same image (default 15) by more than `mask_threshold` % (default 25). The table shows the mask size; an empty mask (< 16 px) fails. |
| `ref_offset` | `[dx, dy]`: the reference is cropped at `rect + offset`, the render at `rect` (content a capture artifact moved, see win-settings-general) |
| `expect` | `reference` (default) or `background`: compare against the scenario's flat `#rrggbb` background (regions that must stay empty) |

`<out>/<name>.regions/mask-<region>.png` shows the mask of masked regions; the
renderer's output goes to `<out>/<name>.log`. `selftest.sh` uses scenarios
without `ref_offset`/`expect: background` regions (those compare against
something other than the plain reference, so `UIDIFF_RENDER_CMD=cp` cannot
pass them).

## Scenarios

The JSON files are generated: edit `tools/uidiff/gen_scenarios.py` and run
`python3 -I tools/uidiff/gen_scenarios.py`. Region rects come from each reference's
`<name>.rects.json` (viewport rects of the page's own elements, written by `capture-v7.js`),
measured on the PNG only where no element exists (the death screen rows).

Menu (1280x720 unless noted; `#uiBase` scale 0.579 for v7's scaleUI 0.7): `menu-1280`,
`menu-1920`; windows `win-settings-{general,controls,display,render,game,sound}` (advanced view),
`win-settings-basic`, `win-servers`, `win-loadout`, `win-classes` (Change Loadout's Class tab: v7
has no Select Class window), `win-mods`, `win-profile-login`, `win-keybinds`, `win-host`,
`win-spray`, `win-games`, `win-shop`, `win-join`, `win-download`, `win-ranked`, `win-attach`,
`win-reticle`, `win-challenges`, `win-changelog`, `win-host-type`, `win-server-finder`. Window
scenarios compare the dimmed login button, menu items and logo, the window's top 82 px (tabs,
search) and its body. Bodies whose content comes from a server (Servers rows, the Custom Games list)
are skipped.

In-game (v7's sandbox, Free for All on Kanji, local player `Local User`): `ingame-plain`,
`ingame-scoped`, `ingame-reloading`, `ingame-scoreboard`, `ingame-chat-open`, `ingame-killfeed`,
`ingame-death`, `ingame-death-self`, `ingame-endscreen`, `ingame-pause-esc`. HUD regions follow
v7's elements (`#timerHolder`, `#matchInfo`, `#pingDisplay`, `#chatInputHolder`, `#chatList`,
`#bottomLeftPlayer`, `#weapHolder`, `#ammoHolder`, `#killFeed`, `#centerLeaderDisplay`, the death
screen rows) plus the crosshair. Skipped: what v7 draws on its overlay canvas or what changes per
run (`#1 VICTORY`, the +score popup, Time Survived).

Hub: `hub-{maps,feed,leaders,market,mods}` and the `hub-login` / `hub-settings` popups. The
reference has no social server (every view is loading), so only the navigation bar, the store
badges, the loading message and the popups are compared.

### Backgrounds

The references are captured over a flat page background (the 3D canvas is transparent), so
the scenarios render over the same colour: `#d6c6c6` for the menu and windows, `#000000` in game
(including `ingame-pause-esc`, where the menu covers the hidden scene).

### anim_time

`capture-v7.js` pauses every CSS animation of the reference at 800 ms
(`document.getAnimations()`, `currentTime = 800`). The renderer starts the page's animations at
clock 0 and captures at `k * 1.6 s + anim_time`, so every scenario uses `anim_time` 0.8.

### References

`node tools/uidiff/capture-v7.js [--out DIR] [name...]` recaptures the references (`--list`
prints the names, `--probe "<js>"` enters the sandbox game and prints an expression). It serves
v7's `public/` read-only and never runs v7's npm server; how each state is reached (pointer-lock
shim, sandbox bots and kills, the stubbed social socket, the mocked server list) is in
`doc/ref/krunker-v7.1.4/README.md`. Needs `chromium-browser` (`KN_CHROME=...`) and
`net-bridge/node_modules` (ws).
