# tools/uidiff

Per-region pixel diff of the native HTML/CSS UI (the Krunker v10.0.3 page, `ui/`) against the
Krunker v10.0.2 reference captures in `doc/ref/krunker-v10.0.2/` and `doc/ref/krunker-v10/`
(captured from the offline v10 sandbox: `capture-v10.js`, `capture-v10-static.js`,
`capture-v10-rewards.js`, `capture-v10-nowplaying.js`, `capture-v10-classbox.js` (the skinned class box), `gen_v10_ingame.py`, `v10_social_ref.py`). The v7.1.4 page and its
scenarios are gone (the target is Krunker v10.0.3; the references are the v10.0.2 captures); the sections below that describe
`capture-v7.js` and the v7 scenario names are history, and `doc/ref/krunker-v7.1.4/` /
`doc/ref/krunker-v3.1.1/` stay as references. A scenario without a regions file is a render
smoke (`make smoke-ui`), not a diff. KrunkNative-only
UI with no Krunker counterpart (the `kn-*` scenarios: the Object Manager window and its HUD rack,
the populated top-right HUD stack `kn-ingame-topright[-old]`: leaderboard, every stat counter and
the kill feed stacked)
diffs against KN's own renders in `doc/ref/krunknative/` (llvmpipe, made with `--render-scenario`
from the same scenario): a regression baseline, not a parity reference. The v10 popups that the offline sandbox
cannot open without running Krunker's JS keep KN baselines too, checked by hand against Chrome: the
emoji and symbol scenarios `kn-lb-grid` (the Leaderboards category cards), `kn-turf` (Overview),
`kn-turf-boards`, `kn-turf-howto`, `kn-turf-history` (the KD ∞), `kn-ranked-sort` (the sort arrow),
`kn-ranked-error` (❌) and `kn-ranked-found` (✨), and a guest's Unclaimed Rewards box `kn-guest-earned-end` /
`kn-guest-earned-menu` (the v10 sandbox captures are of a guest with no balance; `state.guestEarned` stands in
for the ledger of `client/src/ui/v10_rewards.c`, checked the same way: `client/tests/guest_earned_e2e.py` also
checks its amounts and its glow). Each icon was compared with headless Chrome 152 drawing
v10's own markup and svelte.css at the box KN's layout gives it (`KRUNKNATIVE_SCENARIO_DUMP`), and the
font Chrome uses per glyph read with CDP `CSS.getPlatformFontsForNode` (ui/README.md, the emoji row). The counters' parity
references are `ingame-counters[-ds][-wide]` (capture-v10.js turns them on through v10's settings
boot: all five, or Deaths + Streak; `-wide` with a kill feed row wider than the leaderboard, so the
counters flow beside the float), and `menu-host-type` (`doc/ref/krunker-v10/menu/`) is the Host
Game window with its Community Games card. `make uidiff` builds `client_bin`
with the in-house engine (`HTML_ENGINE=ON`) and renders every scenario through the
real client and its C UI logic (see [Rendering with client_bin](#rendering-with-client_bin)):

```sh
make uidiff                                  # all scenarios
make uidiff SCENARIOS="main-menu customize-class" # a subset
```

`tools/uidiff/uidiff.sh <client_bin> [scenario-name...]` is the pipeline underneath;
without `UIDIFF_RENDER_CMD` it calls `<client_bin> --render-scenario <scenario> <out>`.

Requirements: bash, ImageMagick (`magick`, or `convert` + `compare`), python3, and
Mesa llvmpipe with either `xvfb-run` (Xvfb) or libEGL (`UIDIFF_HEADLESS=1`).

The references and the tuned thresholds come from one machine: the old Fedora desktop's llvmpipe, with
Liberation Sans as the system fallback font (`client/src/html/engine/text.c` now tries it first; DejaVu
Sans first made Debian hosts lay Helvetica / Arial text out wider). On the Debian 13 devbox (Mesa 25.0.7,
LLVM 19.1.7) on 2026-11-22, solid fills and text are pixel-identical to the desktop's own captures
(`kn-servers-official-mixed`: 2552 differing pixels of 921600 with no fuzz), yet 93 of 942 regions fail:
83 of them pass with 4 to 8 points more fuzz (small colour differences in blended and scaled images,
e.g. `game-updating` credit: 124 pixels at fuzz 6, none at 8). The likeliest cause is the llvmpipe
version (the desktop's was not recorded). The structural failures of that run were root-caused on
2026-11-23 (see "Failing-region triage (2026-11-23)" below); after those fixes the devbox, run as four
shards, gives 763 passed, 88 failed, 91 skipped (HEAD before them: 759 / 92 / 91 in the same run),
and 787 / 64 / 91 after the translated-window snapping fix (791 / 60 / 91 after the percentage and
transformed-origin fixes of "Failing-region triage, round 3"; 794 / 59 / 89 after round 4; on
main 4b5ef143, with the death screen failing since a73e24b8: 785 / 69 / 89, and 789 / 65 / 89 after
round 5; 800 / 54 / 89 with round 5 on 5f2e9442, which fixed the death screen).
`menu-host-type` was a layout of its own: the Rent a Server card's 15 px margin moved the centred window a
fractional 31.3 px at the #uiBase scale (fixed 2026-11-23: 14 px, `rent_servers_ui.c`; the scenario also needed the
sandbox menu its reference shows).
Compare a run on another machine with this baseline, not with zero failures, before treating a
colour-only failure as a regression.

Renders are time-zone independent: `ui_scenario_begin` sets `TZ` to US Eastern (`EST5EDT`), the zone
the references were captured in, so local dates (the tip popup's "Sat Sep 08 2001") match on a UTC host.

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
  preview canvas stays empty (transparent: the page's shade covers its box like the
  rest of the background);
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
| `discord` | `account_links_discord_scenario()`: the Connections tab's already-loaded `getStatus().data`, before the window mounts it |
| `blur` | after `windowActions` / `windowClicks`, blurs `document.activeElement` for a reference captured without a focus ring |
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
| `windowScroll` | object `{id: top}` | scrollTop of the window's scroll containers once `windowActions` ran (clamped, so `1000000` is the end): `{"knOmToolsList": 1000000}` (kn-om-tools-short). A key starting with `.` is a selector (its first match), for the v10 Svelte popups that have no ids: `{".my-clan-tab.svelte-51j5cz": 800}` (kn-turf-history) |
| `creationVote` | object `{map: {id, name, creator}, mod: {id, name, creator}}` | the menu's vote on the hosted community map (`#mapVote`) and the loaded mod (`#modVote`) as `creation_vote_set_map` / `creation_vote_set_mod` set them (NET_EXT_KN_CREATION_INFO, a mod load); id 0 = none (`kn-map-vote`, `kn-mod-vote`) |
| `connected`, `ready` | bool | net state for `#instructions` (CLICK TO PLAY when both true) |
| `updateArt` | int | the `img/menu-bg/<id>.png` (and its credit) the Game Updating screen shows; default id 0. `seq-game-updating` pins what v10's seeded `Math.random` picked in the capture (4) |
| `inGame` | bool | HUD visible when `inGame && !menu`; with `menu` also true = ESC pause |
| `mapName`, `modeName`, `region`, `name`, `gameName`, `classIndex` | string / int | state glue (`#curGameInfo`, `#menuRegionLabel`, `#menuClassNameTag`, `#gameNameHolder`, class box) |
| `anim_time` | number | animation clock (s) of the captured frame (see [anim_time](#anim_time)) |
| `anim_seek` | number | ms: the capture frames pin every CSS animation like a Web Animations capture (`capture-strike.js` FINISH_ANIMS): finite ones and transitions finished, infinite ones at currentTime = `anim_seek`; the Krunker Strike references use 800 |
| `hud` | object | the `hud` payload of `ui/js/hud.js`: `health, maxHealth, healthSegments, ammo, ammoMax, ammoLimit, melee, reloading, reloadPercent, timerText, kills, deaths, score, weaponName, weaponIcon, classIndex, scoped, fps, aim, spread, scoreboard, ping, loadout[{name,icon,melee}], loadoutIndex` |
| `chat` | array of `{text, color}` | `#chatList` rows (server messages are `#fc03ec`); a row may carry `name`/`msg` for player lines |
| `chatOpen`, `chatInput` | bool, string | `#chatInput` focused with the given text |
| `killfeed` | array | `killfeed` payloads (`killer, victim, weapon, weaponIcon, killerIsYou, victimIsYou, headshot, age_ms`) |
| `scoreboard` | array of `{rank, name, score}` | `.newLeaderItem` rows shown while `hud.scoreboard` is true |
| `dead`, `killcard` | bool, `{killer, weapon, level, score, classIndex, stats}` | death screen (`#deathUIHolder`; `stats` = v7's `[timeAlive ms, shots, "acc%", kills, score]`), HUD holders hidden |
| `endscreen` | `{place, text, nextRound}` | end screen (`#endUI`), HUD hidden |
| `hit` | `{elapsed_ms, kill}` | hit marker `elapsed_ms` after the hit |
| `ranked` | `{rating, party, clientId, tab, cooldownMs}` | the Ranked window (27) without a brain (with `account`): `rating` = a `/mm/queue/rating` answer, `party` = a `/mm/queue/poll` answer, `clientId` = our member, `tab` 0 Play / 1 Season (`ranked_ui_scenario`) |
| `spin`, `spinMulti`, `spinResult` | int, bool, `{rand, item}` or `{rand, kr}` | the store's prize wheel screen without a brain: `prizeWheel(spin, null, spinMulti)` with noAnim (the wheel at its full size), then optionally the spin's reply (rolls `rand`, skins `item`: numbers, or arrays for a multi-spin) with the spin itself skipped: the won item's reveal, its 3D model in `#spinItemCanvas` (`store_ui_scenario_wheel` / `_result`; the reveal needs `account`). `{rand, kr}` is the Free KR wheel's prize (index `kr`, the KR coin); `"live": true` keeps the spin running on the scenario clock (a sequence with `anim_origin` `selector:#spinItem` captures the reveal and the item's fly-in) |
| `wildOffer` | `{offer, now, notify, view}` | the account's v10 Wild Offer without a brain (the brain's `wo` reply, v10_wild_offer.c): `offer` in the Store's shop front (Eterno Shop tab), `now` pins Date.now() (epoch ms) for its countdowns, `notify` pushes its notification, `view` opens its item view |
| `spinHistory` | array of `{ind, dte, fds}` | with `spin`: a `spinH` reply, the Spin History popup opens |
| `matchVotes` | array of `{mode, map, name, votes}` | the end screen's vote cards (a `MATCH_VOTE_STATE` packet through `lobby_events_handle`), with `endscreen` |
| `endTab` | `"leaderboard"` / `"performance"` / `"medals"` / `"rewards"` | the end screen's tab once its results show (`GUI.endScreen.changeTab`): the Leaderboard table with `console` `["bot 3", "end"]` and an `anim_origin` `"end"` sequence |
| `charge` | number or `{prog, over}` | in game: the weapon charge bar (interactMProg) at this interMProg, or the overCharged cross (`build_ui_scenario`) |
| `interact` | string | in game: `#interactMsg`'s innerHTML, shown through `ui_interact_tick` as if the player stood at an interactable (`ui_interact_scenario`) |
| `console` | array of `"cmd"` / `{at_ms, cmd}` | developer console commands (`ui_console_exec`) once the state is applied, or `at_ms` of virtual time later: `kill`, `end`, `bot 1` + `{"at_ms": 1500, "cmd": "kill @bots"}` (the sandbox server's own packets drive what follows) |

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

| `pending` | `"<owner>: <what is missing>"`: skipped like `skip`, the text printed: the native side is not built yet (UI-PARITY-AUDIT section 4); the owner removes it with the implementation. On a scenario (top level) the whole scenario is reported as pending without rendering |

`<out>/<name>.regions/mask-<region>.png` shows the mask of masked regions; the
renderer's output goes to `<out>/<name>.log`. `selftest.sh` uses scenarios
without `ref_offset`/`expect: background` regions (those compare against
something other than the plain reference, so `UIDIFF_RENDER_CMD=cp` cannot
pass them).

## Frame sequences

Single frames test layout; sequences test timing (durations, delays, easing, setTimeout
chains). A scenario with `anim_times` renders a frame at each time after an origin event and
diffs it against the reference frame captured at the same time after the same event in v10:

```json
{
  "ref": "doc/ref/krunker-v10.0.2/sequences/death",
  "size": [1280, 720],
  "background": "checker",
  "anim_times": [0, 250, 500, 750, 1000, 1500, 2000],
  "anim_origin": "death",
  "state": { "inGame": true, "mapName": "Kanji", "console": ["kill"] }
}
```

- `ref` is a directory of `<t>.png` (+ `meta.json`: origin, the virtual clock of each frame,
  the per-frame `probe` values, notes); the renderer writes `<out>.<t>.png` per time (and the
  last frame as `<out>` itself).
- `anim_origin` (`client/src/ui/ui_scenario.c`): `applied` (default: the first frame after the
  state is applied; timed state such as `killfeed` ages and `hit` is set for that frame), `cycle`
  (the first multiple of 1.6 s after that: every infinite menu animation at its start), `boot`
  (the page's boot as `state.boot` raises it again: the first frame after the state, like `applied`;
  clock 0 itself comes before the page, its map and the local server are up), `init` (the first INIT), `spawn`, `death` (the local player's), `kill` (its next
  kill), `end` (the match end), `selector:<css>` (the first frame laying out a match: a page
  timer or packet). Frames land exactly on origin + t on the 60 Hz virtual clock. Every native
  origin is the frame that processes the event and draws it; on the v10 side a sequence with a
  `trigger`, no `origin` expression and `drawnOrigin: true` (kill, hit, end-vic, end-results) starts at
  the end of the first 16.7 ms step after the trigger (the rAF that draws the overlay canvas: hit
  marker, score popup, victory title), not at the trigger itself, whose screenshot shows the DOM
  changes but not yet the canvas.
- `background: "checker"` (`checker[:cell[:#light:#dark]]`, default 32 px `#8a8a8a` / `#343434`)
  replaces the hidden 3D view with a texture, so `backdrop-filter` and `filter: blur()` show; the
  v10 capture paints the same pixels (`capture-v10-seq.js` `checkerPng`).
- Regions apply to every frame unless they list `times`; `"at": {"500": {"max_pct": 5}}`
  overrides keys for one frame.
- `timing` entries are the timing assertion:
  `{"name": "card", "rect": [x, y, w, h], "threshold": 0.5, "tolerance_ms": 16.7}`. For the
  reference and the render, the change of the rect from the first frame (mean absolute
  difference) is followed over the frames; the time where it first reaches `threshold` of its
  largest value (linearly interpolated between frames) must agree within `tolerance_ms` (one
  60 Hz frame by default). A render that never changes there fails; a reference that never
  changes is a bad rect. `pending` / `skip` work as on regions.
- Outputs: `<name>.<t>.png`, `<name>.<t>.diff.png`, `<name>.<t>.sbs.png`,
  `<name>.<t>.regions/` per frame and `<name>.strip.png` (reference frames over rendered ones).

References: `node tools/uidiff/capture-v10-seq.js [--out DIR] [--list] [name...]` (default
`doc/ref/krunker-v10.0.2/sequences/`), the offline v10 sandbox of `capture-v10.js` with
CDP virtual time: JS timers, `Date.now` / `performance.now` and the overlay canvas'
`requestAnimationFrame` follow virtual time; Chromium's animation timeline is frozen
(`Animation.setPlaybackRate` 0) and every CSS animation / transition is set to its virtual age
before each step and screenshot. `--list` prints every sequence, the ones the sandbox cannot reach
with the reason (never faked). The sequences (`seq-<name>` scenarios):

| Sequence | Origin (v10 / native) | What it shows |
|---|---|---|
| `boot-curtain` | first INIT / `init` | `#instructionsFadeBG` fading 0.7 s ease-in-out |
| `loading-map-name` | the loading screen of `?play=` / pending S1 | `#loadGamNm` "Loading Kanji ..." |
| `click-to-play-grow` | animations restarted / `cycle` | CLICK TO PLAY `grow` over one 1.6 s cycle |
| `auto-respawn` | "Auto Respawning in 3" / `death` | the 1 s countdown (`state.autoRespawn`: S1) |
| `lost-connection`, `game-updating` | the `error` packet / `applied` (`connectError`) | the error screens, no open transition |
| `death` | death screen shown / `death` | self-kill card over the checker, moveup 1.5 s |
| `kill` | the kill / `kill` (`bot 1`, `kill @bots`) | kill feed (gone after 4 s), score popup, medal |
| `hit` | DO_DAMAGE / `applied` (`state.hit`) | hit marker (+ damage number in v10) |
| `emoji-panel`, `game-announcement` | panel / banner laid out / pending S5 | slideInUp, the banner |
| `end-vic`, `end-results` | the end packet / `end` (`console end`) | victory title every 100 ms, the results |
| `end-collect-button`, `end-rewards-overlay` | injected `otherData.items` / pending S4 | Collect Rewards pulse, card flip + fly |
| `notification-register` | the 45-60 s guest timer / pending S5 | notification center slideIn + countdown |
| `tooltip-loadout` | pointer on an item / pending S5 | createTip |
| `mobile-menu`, `menu-1000` | / `applied`, `cycle` at 1000 px | the sandbox menu (`state.sandbox`: the capture's custMap runs v10's in-page sandbox), at 1280 and 1000 px |

Unreachable offline (listed by `--list`, not captured): `nuke-flash` (a 25-streak pickup drawn
on the overlay canvas), `ranked-leaderboard-open` (signed-in ranked data), `spin` (the server's
spin reply), `store-tooltip` (the signed-in store list).

## Scenarios

The JSON files are generated. The v10 in-game ones (`ingame-*`) come from
`python3 -I tools/uidiff/gen_v10_ingame.py`, which takes its states from `gen_scenarios.py`'s table
and applies v10's values (edit both there); `gen_scenarios.py` run on its own writes the retired v7
set and deletes the scenarios it does not know (history, do not run it). Region rects come from each reference's
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

Prize wheel (`state.spin`): `win-spin` (1280x720), `win-spin-1920`, `win-spin-1024` (1024x768),
`win-spin-portrait` (720x1280) compare the wheel's disc and the odds box (`#spinInfo`); the
`win-spin-item*` variants are the won item's reveal (signed in, Digital AK), comparing its name card
and buttons (v7 draws the item in 3D: skipped); `win-spin-multi` a Starter 10x spin's first reveal,
`win-spin-history` the Spin History popup, `win-shop-spin` / `win-shop-wheels` the Store's wheel cards
signed in with spin tickets (`account.skins`). `tools/kref/spin_e2e.py` spins for real against kn_brain.

In-game (v7's sandbox, Free for All on Kanji, local player `Local User`): `ingame-plain`,
`ingame-scoped`, `ingame-reloading`, `ingame-scoreboard`, `ingame-chat-open`, `ingame-killfeed`,
`ingame-death`, `ingame-death-self`, `ingame-endscreen`, `ingame-pause-esc`, `ingame-interact`
(Freight's "Press [G] to pickup Junk" trigger prompt: v7's own updateInteract writes it in the
reference), `ingame-charge` / `ingame-charge-1920` (the weapon charge bar, the overlay canvas's
interactMProg, half charged), `ingame-charge-over` (past full: red, thicker, shaken) and
`ingame-charge-cross` (overCharged), the last two over grey to show the bar's back and shadow
stroke (`state.charge`; v7's reference pins the local player's interMProg / overCharged). HUD regions follow
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

### KrunkNative baselines re-rendered

A `doc/ref/krunknative/` baseline is re-rendered only after a side-by-side check that every changed
pixel follows an intended change; pixels that differed for any other reason keep the old reference.

| Baselines | When | Why |
|---|---|---|
| `kn-guest-earned-end`, `kn-guest-earned-menu`, `kn-lb-grid`, `kn-main-menu-connect`, `kn-menu-official-guest`, `kn-menu-official-header`, `kn-ranked-error`, `kn-ranked-found` | 2026-10-25 | `menu_scale_base` now writes #uiBase's CSS width/height exactly as v10's `scaleBase` (`deob-10.0.3.js:241726-241731`) instead of rounding them up to whole CSS px, so right-anchored and centred content moves ~1 CSS px (0.6 device px) onto v10's positions. Only those subpixel shifts changed, checked side by side (`main-menu` action-cards 16.68 -> 0.63 %, logo 8.46 -> 2.14 %, both market popups -> 0.35 %). `kn-v10-menu.css` gives #instructionHolder a 1px transparent right border so its shade still covers the last device pixel, as Chrome's does (`menu_bleed_e2e`). |
| `kn-om-*` (10), `kn-om-tools-short` | 2026-10-15 (uidiff-red) | 1f32a8bc made the window a flex column and 422e6a02 wraps tile, tool and gun names; krunknative.css `.knOmMain` then lets the whole Tools pane set the row's height (a 3-tile search showed one tool). Only the window's box (x 290-990, y 140-580) is new; the chat behind it keeps the old pixels. Region rects follow the new boxes (`KRUNKNATIVE_SCENARIO_DUMP='#menuWindow'`). `kn-om-weapons` / `kn-om-search` are rendered with engine-replaced-pct (a percentage-width `<img>` adds only its border and padding to min-content, as in Chrome), which narrows the weapon icons. `kn-om-tools-short` is now 1920x600 with UI Scaling off (with scaling on the pane always fits) and has regions. |
| `kn-ranked-sort` | 2026-11-23 | The baseline (a956c0fc3, 2026-10-15) predates 2eef54563, which made the ranked leaderboard sort descending as v10 does: `sortColumn` starts at score / ascending and is toggled once on open (`deob-10.0.7.js:110388-110394`, 110414), so the header shows U+2193 (110432; v10.0.2 `game.js` 108074 / 108099 / 108115 is the same). Only the arrow and the row order changed. |
| `kn-image-collection`, `kn-image-collection-new` | 2026-11-23 | image_collection.c now draws v10.0.7's own markup and wording (`deob-10.0.7.js:214718-214756`: Image Collection / Add Image, the Backgrounds ... Icons tabs, the Select / Delete tiles with their Custom / Default line, the built-in Midnight Snow, the New Image form) instead of Box Office's own (My Images, kn-alt.css's grid, a stacked form). The whole popup changed, by design; the menu behind it is the old reference's. The regions follow the new popups' boxes. |
| `kn-editor-ai` | 2026-11-24 | b19dc9ab intentionally put the KN-only panel on the editor's GameFont; `layout_finish_scroll` now clamps against clientHeight, so its empty list no longer scrolls 16 px by its padding. Only the panel (x 1230..1611, y 38..928) was re-rendered. Chromium on the captured DOM at dpr 1 agrees with the native header / sign-in / empty / composer regions to 0.111 / 0.178 / 0.067 / 0.340 %; every pixel outside the panel is the old baseline. |
| `kn-servers-official-mixed` | 2026-11-23 | The empty `#kioStatus` line no longer collapses the gap under the tab strip to 22 px (see the 2026-11-23 triage); only the lobby list moved, 8 CSS px down. |
| `kn-rent-regions`, `kn-rent-region`, `kn-rent-mine`, `kn-rent-server`, `kn-rent-owned` | 2026-11-23 | rent_servers_ui.c now draws v10.0.7's buyServ markup and wording (`deob-10.0.7.js:219853-219935`): no Box Office "Rent a Server" header, region cards without a thumbnail box, tab 3's Server Region / Time remaining / Rental Duration or Extend Time / Select or Change Game / terms / Server Cost, and the message line and buttons outside the panel. The whole window changed, by design; only the region rect (280,50 720x620) was taken from the new render, every pixel outside it keeps the old reference. |
| `kpd-replay-bar`, `kpd-replay-vote`, `kpd-replay-case` | 2026-11-24 | Round 13 replaces Box Office's replay wording with v10.0.7's (`deob-10.0.7.js:229603-229844`): RESOLVE / HACKER? / SUSPECT and KPD Demo Review / Copy demo link. Only the changed text and its layout were taken from the new renders. |
| `game-updating`, `ingame-playerlist-spectate-centred`, `kn-end-share-pop`, `kn-image-collection`, `kn-image-collection-new`, `kn-lb-grid`, `kn-om-*` (10), `kn-ranked-error`, `kn-ranked-found`, `kn-ranked-sort`, `kn-rent-owned`, `kn-rent-server`, `kn-servers-official-error`, `kn-servers-official-mixed`, `kn-store-kio-eterno`, `kn-store-kio-ranked`, `kn-tip-item`, `kn-turf-boards`, `kn-turf-howto`, `lost-connection`, `lost-connection-inactive`, `v10-market/{browse,create,detail,sales,trading}`, `v10-wallet/skins` | 2026-11-23 | Translated windows snap in their parent's space (sub-pixel placement, see the 2026-11-23 triage). Only the failing regions were touched, and in them only the pixels that the paint change moved: the render of the same tree without it (base) against the render with it; every other pixel keeps the old reference (several have menu backgrounds that changed since, e.g. the market's Daily Spin row). |
| `kn-customize-colors`, `kn-image-collection` | 2026-11-23 | `layout_block.c` `compute_overflow` adds a scroller's end padding to its in-flow children's boxes only, as Chrome's ScrollableOverflowCalculator does, so a descendant that ends inside the padding no longer makes the scroller overflow and `scroller_layer_snap` no longer moves its contents to a whole device pixel. Chrome on both DOMs: `#loadoutContent` scrollHeight 909 = clientHeight, `#genericPop` 542 = 542. Only the pixels the fix moved (base render against new render) were taken: the colour rows (x 558-1069, y 136-632) and the popup (x 346-933, y 168-473); against Chrome's capture of the same DOM at fuzz 12 they went 2.94 -> 0.44 % and 22.7 -> 20.5 %. |
| `v10-market/browse`, `v10-market/inventory` | 2026-10-15 (uidiff-red) | the `.market-grid` box only: 4260815e (`all: unset` drops the Buy button's UA border) and 1657aa83 (`aspect-ratio: 1` on `.market-card-shell`), bisected. The left strip (the v10 menu column behind the popup, ~0.37 %) stays as it was in all eleven market baselines. |

### Paint pages (`make uidiff-paint`)

`tools/uidiff/paint.sh <ui_test>` renders each `tools/uidiff/paint/*.html` page with
`ui_test render-page` (the whole `ui/` tree, its `index.html` swapped for the page) and pixel-diffs
it against Chrome 152's capture in `doc/ref/krunknative/paint/`: `flip-mid` (a mid-flip
`.reward-card` with `rotateY` under `perspective`), `mask-icons` (`mask` / `-webkit-mask` with
`url()` and `linear-gradient`, `background-clip: text`, `color-mix()`, `outline-offset`) and
`backdrop-tex` (a 30% black panel with `backdrop-filter: blur(10px)` and a 24px radius over the
battle-pass art, like `.death-ui-bottom`). Chrome's capture: `google-chrome --headless=new
--no-sandbox --disable-gpu --hide-scrollbars --window-size=1280,720 --screenshot=<png> <page>`.

### Failing-region triage (2026-10-25)

After the `scaleBase` fix the suite had 120 failing regions. They triage into these groups; "pending"
on a region means the reference itself is out of date or shows something KN deliberately does not do,
and the note says which.

**Fixed in this pass**
- **Secondary level/custom requirements were skipped offline.** `customize-secondary` showed every
  secondary Unlocked where v10's offline sandbox shows Customs Only and Req lvl. v10 skips
  `customOnly` and the `minRec` tail only in a custom game, never in its offline sandbox
  (`deob-10.0.3.js:112377`). `secondary_req_ok` (client) and `game_weapon_req` /
  `game_weapon_level_ok` (server) now treat the offline sandbox as not custom, and the pick falls
  back to the Pistol as `updateSecIndex` does. `customize-secondary` 30.18 -> 2.79 %.
- **`kn-settings-quickplay`**: the section starts folded since 7ab22d6fa, and the presets row moved
  the rest. The scenario now opens the folder with a real DOM click (`state.windowClicks`, new in
  ui_scenario.c), the region rects were re-measured and the baseline re-rendered: 5/5.
- **Object Manager baselines** (11 scenarios, 45 regions): stale since 9c793fb0b removed the Tools
  pane's Release row (an intended Physics Gun change), so they were re-rendered: 45/45.
- **Outdated references marked pending**: `kn-menu-popular-official` (Popular Now is Box Office only
  since 95bf5b912), `kn-ranked-home` `rk-maps` (10.0.3's ranked pool drops Eterno Simulator for
  Frontier), the ad-button regions (`hub-settings`, `settings-game`, `settings-advanced-controls`,
  `settings-mobile-2`), the Account Manager strip (`main-menu-buffs` / `-krsale` / `-popular`,
  `main-menu-glow-*`, `menu-mobile*` header-left), `store-home` (the reference has no Heroic Spin
  art), the editor panels (Solid Ramp Sides, Snapping ON, the Shared script tab). The hub
  `editor-badge` difference is anti-aliasing only, so its tolerance is 2.5 % instead of 1.5 %.

**Still open, and why**
- **Server browser** (`kn-servers-official*`, `kn-servers-carousel`): the krunk.io / krunker.io tab
  bar and the download button are intentional and shift every region. The footer's counters are ported
  (2026-10-25): the brain answers `socialPlayerCount` and `editorPlayerCount` besides `playerCount`,
  and editor windows heartbeat `/api/editor/presence`; the scenarios' fixed list has no counts to show.
- **End screens**: fixed 2026-10-25. The signup banner no longer moves down to keep a 42 px gap
  under the Double XP button when the button is already above it (v10 keeps its 850 px place unless
  the card reaches it), and the END hides `#infoHolder` in game during the title and the results
  (the direct-results path keeps its footer). `seq-end-results` passes, timing 4.7 ms (was 135.9).
  The end-vic and end-results references were re-captured on 2026-10-25 from the official v10.0.3
  sandbox (`KN_V10_PUBLIC=~/jails/kru/src/Krunker_v10.0.3 KN_V10_CLIENT_JS=~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js
  node tools/uidiff/capture-v10-seq.js --out <dir> end-vic end-results`), after the old mirror and
  numeric scope hook were found missing. The capture tool now has a v10.0.3 scope hook and supplies
  the decoded bundle's global timer alias. The 200 ms title is 0.000 % (was a pending extra-rAF
  reference frame), and title timing is 1.8 ms. End-results captures 1000/1200/1400/1600/1800 ms as
  well: sparse 0/2000 ms frames made its timing crossing depend on the final results' pixels; with
  the denser reference it passes at 13.4 ms (reference 656.6 ms, render 643.2 ms).
- **Battle Pass** (`rewards-bp-home`, `rewards-bp-vip`, `rewards-bp-chal`): fixed (2026-10-25). The
  scenarios already carried the pass (`state.account.battlepass`), but the active-account change
  after the stand-in login wiped it before the window rendered, so every one showed "Loading Battle
  Pass". `battlepass_ui_scenario` now pins its fixture against those resets and re-renders the
  window; `client/tests/bp_scenario_e2e.py` (ctest `bp_scenario_e2e`) asserts the item count, tier,
  BPP, claimed cards and tab from the scenario's state line. All 6 regions pass (bp-header 0.699 /
  0.746, bp-levels 2.068, bp-buttons 3.606 / 3.861, window-body 0.579).
- **`market-junkyard` `register`** (fixed 2026-11-19, 5.70 -> 0.00 %): a real markup error, not a stale
  reference. v10 renders the guest's Register line, the Loading / No Recipes status and the cards inside
  `.craft-grid` (a wrapping flex row, `deob-10.0.7.js:138020-138054`; a fresh offline-sandbox capture shows
  the same DOM), so the line shrinks to its text at the grid's left; `v10_junkyard.c` had put it before the
  grid, where its `text-align: center` centred it.
- **`kn-market-blackm` `popup`** (fixed 2026-11-22, 20.6 -> 0.00 %): a stale reference. Its reference is
  KrunkNative's own capture, made from a hardcoded 8-item table; the grid now lists v10.0.7's 36 Black Market
  items (`blackM`, neither `hide` nor `free`: `deob-10.0.7.js:137663-137676`) from
  `economy_black_market_table.c`. Puppet Master is hidden in v10.0.7, so the scenario's account owns the
  1 Year Cape instead, to keep an Owned card in the capture.
- **Not individually root-caused here**: `ranked-shop-flying`, `ingame-endscreen-collect-raid`, `kn-ranked-sort`, `kn-servers-official-error`
  and `kn-servers-official-mixed`, and the `strike/*` scenarios (the Strike flavour, not v10 parity).
  All of them are covered by the 2026-11-23 triage below.

### Failing-region triage (2026-11-23)

The structural (not fuzz-only) failures of the 2026-11-22 devbox run. Before -> after, devbox llvmpipe.

**Fixed**
- **`kn-servers-official` top-groups** (15.36 -> 2.71 %) and **region-groups** (6.58 -> 2.19 %): two
  causes. The region was wrong: its rect began at y 227, inside KN's krunk.io / krunker.io tab strip
  (which ends at 240), and compared the strip's bottom with v10's window header. It now starts at 250
  (offset -16); window-header and footer were re-fitted the same way. The render was wrong too: the
  status line under the tab strip (`#kioStatus`, `div.kioSep` with margins 24 / -8) is empty when the
  lobbies are listed, so its margins collapsed through it with the strip's 30 px and the first group's
  20 px to 22 px, where v10's tab strips keep 30 px before the first group (`genList`: `bigMenTabs`
  margin-bottom 30, then `setHed` margin-top 20). The lobby list jumped 8 CSS px between the krunk.io
  and krunker.io tabs (first group at local y 978.6 vs 986.6). krunknative.css `div.kioSep:empty`
  now takes no room; `kn-servers-official-mixed` was re-rendered for it, and
  `kn-servers-official-open` / `kn-servers-carousel` had their offsets re-fitted (the window is 8 CSS px
  taller, so it is centred 2 device px higher and the list moves 4.8 device px down).
- **`kn-ranked-sort` rs-arrow** (0.91 -> 0.00 %): a stale KN baseline showing the old ascending sort;
  re-rendered (table above).
- **`kn-tip-spin` cost** (2.00 -> 0.00 %): time zone. The reference says "Sat Sep 08 2001" (1e12 ms is
  2001-09-09 01:46:40 UTC, the evening before in US Eastern, where it was captured); the UTC devbox
  rendered "Sun Sep 09 2001". `ui_scenario_begin` pins `TZ` to `EST5EDT`.
- **`ranked-shop-flying` flying-number** (6.17 -> 0.00 %): the reference is a composite (KN's
  ranked-shop render as the background, Chromium drawing v10.0.7's flying-number layer over it), and
  its background still read "Region: North America" from before 2eef54563 made Europe the default.
  Re-composed with `KN_CHROME=chromium node tools/uidiff/capture-v10-menu-idle.js --state flying
  --capture <dir> --public ~/jails/kru/src/Krunker_v10.0.7/live --bg <ranked-shop render> --x 896.981
  --y 183.712 --text -500 --t 817`, where `<dir>` holds the live 10.0.7 `index.html` and a
  `states/main-menu.html` of `<div id="uiBase"></div>`. Check: the same capture over a North America
  background reproduces the old reference with 0 differing pixels in the region. `tools/kref/cdp.js`
  falls back to Node's built-in WebSocket when the `ws` module is not installed.
- **`seq-end-collect-button@500` collect** (13.65 -> 1.73 %, pulse timing still 0.0 ms): CSS
  animations started one frame early. A style flush between frames (an observer, a timer) created
  the running animation with the previous frame's clock, so the frame after it was already 16.7 ms
  in; KN's frame at 483 ms matched the 500 ms reference. As in the Web Animations model, a new
  animation is now play-pending (`AnimSystem.defer_starts`, set by the engine) and its start time is
  the time of the next frame that paints it (`anim_start_pending`, called from `html_ui_backend_render` after
  layout and at the start of `anim_tick`). The other sequences' timings are unchanged (death 0.9,
  kill 0.2, hit 0.0, end-vic 1.8, end-results 13.4, grow 0.8, curtain 0.0, flip 0.0 ms).

**Passing on the devbox, nothing to fix**: `ingame-endscreen-collect-raid` (3 of 3 runs),
`kn-servers-official-error` (0.000 %) and `kn-servers-official-mixed` (0.003 % before its re-render).

**Environment, not KN** (wrong, see "Strike text, the static reference and flex shrink" below)
- **Strike `item-selection` / `class-selection`**: the weapon thumbnails are PNG previews
  (`assets/textures/previews/weapons/*.png`, `image-rendering: pixelated`), not 3D. Regenerating the
  Chrome reference on the devbox with `static_ref.py` gives the committed reference exactly (items-row
  0.000 %), while KN's render is 8.3 % off; forcing smooth sampling does not help (8.2 %), and the same
  code passed at 2.4 % on the desktop (4b03e2be). This was put down to KN's GL image sampling on
  llvmpipe; it was layout (a 0.67 px flex shrink), text kerning and the reference's device scale.

**Sub-pixel placement of translated windows** (fixed later on 2026-11-23). With v10-identical
markup (the tab strip and the status line hidden) KN painted the Servers window's sticky header
0.28 device px higher than Chrome (top edge 175.02 vs 175.30) and its rows 0.45 px higher (a group's
4 px top border at 239.84 vs 240.29). Two departures from Blink in `paint.c`, both measured there:
- **A 2D translation kept its own snapping space.** `#menuWindow` is `top: 50%` (layout top 621.55 in
  `#uiBase`, scale 0.579) with `translate(-50%, -50%)`. KN snapped the window's decorations from its
  own origin (621.55 - 319); Blink keeps a translated box's sub-pixel paint offset (subpixel
  accumulation, dropped only for non-translation transforms), so its boxes snap in the parent's space
  at round(621.55 + y) and the translation moves them by its exact amount: (622 - 319 + 112) * 0.579 =
  240.29, the measured row. `box_own_snap_space`: only composited boxes and non-translation transforms
  start their own space. `scroller_layer_snap` takes T from the same snapped origin, which is what keeps
  the scrolled windows (settings) right.
- **A sticky box layerized a scroller without overflow.** The rule came from v7's retired `win-servers`
  capture; v10's Servers window (638 of a 1013 px max height) does not overflow, and Chrome does not
  snap its contents to whole device px (that snap put the rows at 239.84). A scroller is snapped only
  when it really overflows (the hub's `#modsHolder` case stays).

With both, the v10-markup renders match the references almost exactly at offset 0
(`kn-servers-official` top-groups 6.77 -> 0.17 %, region-groups 6.86 -> 0.18 %; `-open`
new-york-header 2.76 -> 0.25 %, new-york-rows 2.96 -> 0.05 %; carousel top-groups 3.04 -> 0.11 %).
On the real markup the full run goes from 763 / 88 / 91 to 787 / 64 / 91 (passed / failed / skipped),
no region from pass to fail. Real-capture regions that now pass: all of `kn-servers-official-open`, the
carousel's header / carousel / names / footer, the customize toolbars (3.08 -> 0.39 %), `rewards-bp-*`
bp-buttons (8.74 -> 0.00 %), `settings-general` / `settings-search` preset-buttons (16.8 -> 11.4 %),
`seq-lost-connection` box (3.62 -> 0.70 %), `popup-kid`, `rewards-signup`, `store-eterno`,
`social-feed`, `menu-host-type` community-card. `kn-lost-connection-official`'s id-line and
extensions-hint had a `ref_offset` of +1 that only made up for the old placement: now 0.000 % at 0.
The `kn-servers-official` offsets are 17 / -17 (16.79 px: the window is 58 CSS px taller, centred).
36 KN-own baselines that encoded the old placement were refreshed (table above).

**Still open**: `kn-servers-carousel` top-groups 4.23 % and region-groups 3.18 % (max 3). With the tab
strip, KN's carousel window is 78 CSS px taller than v10's (48 px strip + 30 px margin, no group margin
to collapse with), so, centred, its header sits 39 CSS px = 22.58 device px higher and the list 22.58
px lower: the best whole-pixel offset (23) leaves 0.42 px. Comparing with the reference resampled at
-22.58 / -22.75 px gives 2.38 / 2.32 % (top-groups) and 1.65 / 1.29 % (region-groups), and the
v10-markup render passes at offset 0 (0.11 / 0.18 %): what is left is the KN-only strip's height, not
the engine.

### Failing-region triage, round 3 (2026-11-23)

Baseline: origin/main ccb73fee (menu ad slots, guest chat, Junkyard, market), four shards on the
devbox's llvmpipe: 787 passed / 64 failed / 91 skipped. After the fixes below: 791 / 60 / 91, no
region from pass to fail. `main-menu` and `menu-mobile*` were left to their owner.

**Fixed**
- **`ingame-mobile` btn-kswap** (13.06 -> 0.000 %, portrait 0.000): float precision in percentage
  resolution. `#mobileKSwap` is `top: 65%` of 390 px; `css_len_value` computed `65 * 0.01f * 390` =
  253.49998, which the box snap rounds down to 253, where Blink's `ValueForLength` (maximum * percent /
  100) gives 253.5 exactly and snaps to 254. `css_values.h` now divides by 100 (the old region note had
  the right observation, an integer top gives 0 %, and the wrong cause). Unit check in
  `test_css_values.c`.
- **`ingame-mobile` / `-portrait` btn-shoot** (8.37 / 8.99 -> 0.000 %): `#mobileShoot` is
  `position: absolute` with `rotate(90deg)` at layout 25.3, 109.2; Chrome paints it from 25, 109. Blink
  drops the subpixel accumulation at every non-translation transform, whatever the position; `paint.c`
  rounded the origin only for `position: relative` (Strike's skewed buttons). The rule now covers every
  non-composited box with a non-translation transform (`test_abs_scaled_origin_rounded`). Side effects,
  all improvements: `ingame-pause-esc` class-card 3.31 -> 0.63 % (now passes), `main-menu-classbox-skin`
  class-box-text 7.71 -> 0.00, `main-menu-nowplaying-long` class-card 5.64 -> 2.93, `menu-mobile`
  loadout 4.31 -> 2.57, Strike class-selection selected-card 6.05 -> 4.96. Largest moves the other way,
  all still passing: `kn-store-kio-eterno` window 0.74 -> 0.99, `kn-guest-earned-menu` unclaimed
  0.00 -> 0.32, `seq-hit@100` 1.52 -> 1.73, `kn-tip-spin` cost 0.35 -> 0.46.
- **`guide` intro-text** (12.04 -> 5.50 %, max 3): its `ref_offset` of -1 made up for the placement
  that fbb8dc02 fixed; at offset 0 the vertical ink centroid matches Chrome to 0.001 px. What is left
  is the glyph x phase (the first line sits 0.12-0.35 px left of Chrome's), still failing.

**Environment, not KN**
- **`hub-*` badges / editor-badge / nav-buttons**: not a real difference in the references, and not
  the hub's new badge cards in `social_ui.c` (the numbers are identical before and after them: badges
  1.843 %, editor-badge 5.745 %, nav-buttons 1.52-1.62 % on every hub page). The regions are scaled
  images (`#editrDB` is `img/editor.png`, the badge row and the nav icons are scaled PNGs) and all pass
  with 4 points more fuzz (fuzz 12: badges 0.48 %, editor-badge 1.58 %, nav-buttons 0.60 %): llvmpipe's
  image sampling, the same as the Strike thumbnails.
- **`settings-mobile-*` preset-select-buttons** (4.76 %, max 4): the "Custom" text of the select sits
  where Chrome's does (ink centroid dy +0.07, dx -0.16 px); the difference is glyph anti-aliasing
  across the whole row (Import / Export / Reset too), and it passes at fuzz x1.5.
- The `timer` (composited `#timerHolder`), `ammo`, `logo`,
  `game-updating` credit, `popup-survey` and `editor-1920` canvas-add (the 3D canvas behind the add
  menu) failures all pass at fuzz x1.5: colour drift as described above.

**Still open**
- **`spin-item*` buttons** (6.3-7.9 %, max 5.5). Two parts. The respin button's Material Icons
  `refresh` glyph is a different shape in the v10.0.2 capture (searching rotations of KN's glyph finds
  no match: the icon font's version, not the spin animation). And Chrome draws the buttons' top and
  bottom borders 2.0 device px thick, where KN draws 4 CSS px x 0.579 = 2.32: the purple box spans
  601.8-634.0 in Chrome, 601.6-634.0 in KN, though both left borders are 2.31 px and the green info
  button's top and bottom match. Chrome rasterizes `#uiBase`'s scaled content in a compositor layer,
  which is the likeliest source; not reproduced in the engine.
- **`boot-curtain-350` instructions** (0.97 %, max 0.5): a KN-own reference (2026-10-14, desktop).
  Only the mid-fade frame differs (`boot-curtain`, `-700` and the `click-to-play-grow-*` frames pass at
  0.00-0.08 %): KN's CLICK TO PLAY is 0.48 px left of the reference and 5 % brighter. With no Chrome
  capture of that frame there is no evidence for which is right, so the baseline is kept.
- **`menu-host-type` window** (3.16 %) belongs to the menu work; **`kn-servers-carousel`** is as
  described in the previous section.

### Failing-region triage, round 4 (2026-11-23)

Baseline: round 3 (75670edd on ccb73fee), 791 / 60 / 91. After the changes below: 794 passed / 59
failed / 89 skipped, no region from pass to fail (every other region within 0.2 points).

**Fixed**
- **Percentages elsewhere**: table column widths (`layout_table.c`, both the auto and the fixed
  algorithm), `font-size` / `line-height` percentages and `zoom: N%` (`css_values.c`) and
  `background-position` / `object-position` percentages (`image.c`) now divide by 100 as Blink does,
  where `pct * 0.01f` gave e.g. 10 % of 720 = 71.99999 and 5 % of 300 = 14.999999. Unit checks in
  `test_layout.c` (`t_table_percent_exact`), `test_css_values.c` and `test_image.c`; no uidiff region
  moved.
- **`guide` intro-text** (5.50 -> 0.19 %), intro (8.37 -> 0.09 %, max 12 -> 3), controls and footer
  (skipped at 18.2 / 18.9 %, now 0.22 / 0.17 %). The first line's 0.12-0.35 px offset was not text
  layout: text-indent and the inline start are 0, GameFont has no kern / GPOS so the advances are the
  hmtx ones, and the Skia-style glyph snap matches. The capture's Basics `<video>` never loaded: it
  is 300 x 150 naturally and the flex row's automatic minimum (content size suggestion = the natural
  width, Blink computes it with `kIgnoreInlineLengths`) lets it shrink from its 700 px style to
  444.7 px. KN used the styled width as the minimum, laid the video out 700 wide, so
  `.tutorialSectionContent` overflowed (931.9 of 910 px) and `scroller_layer_snap` moved its content
  -0.32 CSS px; Chrome has nothing to scroll and no layer. `layout_flex.c` now takes
  `layout_replaced_natural_width` (`layout_block.c`) for replaced items
  (`t_flex_shrink_auto_min`), and the scenario's `"guideVideo": false` renders the unloaded video
  (`v10_news.c`).
- **`spin-item*` buttons** (6.34 / 7.91 / 6.90 / 7.91 -> 5.62 / 7.29 / 6.28 / 7.29 %, still failing
  at max 5.5): round 3's "icon font version" was wrong. v10.0.7's `css/material-icons.css` links
  `fonts/flUhRq6tzZclQEJ-Vdg-IuiaDsNc.woff2`, the same file as KN's (both "Version 1.017",
  512 units / em, and `ui/css/fonts/material-icons.ttf` is its conversion). The reference glyph is
  KN's `refresh` turned ~225 degrees by `#spinAgainBtn > i`'s 2 s spin (the earlier rotation search
  scored the border lines pulled in by the rotated crop, not the glyph). `anim_time` 0.05 (capture
  at 1.6 k + 0.05 s) matches that phase in all four scenarios; 0.8 drew it at 0 degrees.

**Still open**
- **`spin-item*` buttons, borders** (resolved in round 5, which also corrects this paragraph: the
  purple box is the respin button, the cyan one Equip): not a uniform device-pixel floor. Under `#uiBase`'s 0.579 scale
  the 4 px borders are 2.32 device px in KN; in Chrome the purple Equip box's left / right are 2.31
  (as KN), its top / bottom 2.03-2.06, the cyan respin box 2.08 (left) and 2.0 (right). The top
  border differs at its outer edge (601.80 Chrome, 601.58 KN), the bottom at its inner edge
  (631.98 / 631.69); the button text matches to 0.01 px. Floor-snapping every border width would
  break the left / right edges that already match, so nothing changed; the likeliest source is still
  the compositor raster of the scaled layer.

### Failing-region triage, round 5 (2026-11-23): borders whose radius equals their width

Baseline: main 4b5ef143, four shards on the devbox's llvmpipe, 785 / 69 / 89 (the death screen
fails since a73e24b8, left to the menu work). After the change below: 789 / 65 / 89, one region
from pass to fail before its KN-own reference was corrected (`kn-map-vote`, below).

**Measured.** The spin-item capture, as per-pixel coverage of the border colour (KN's band in
brackets): respin (purple) left 514.30-516.61 and right 612.18-614.49 (the same), Equip (cyan) left
648.86-650.94 (648.65-650.97) and right 763.00-765.00 (762.72-765.03), top 601.80-603.86
(601.58-603.90), bottom 631.98-633.98 (631.69-634.00). Snapping each edge to device pixels cannot
give this: it makes every edge whole, where the respin box's left and right edges carry 0.70 / 0.61
and 0.82 / 0.49 of a pixel. To isolate the rule, headless Chromium 1255 (SwiftShader) drew 64
`.button`-like boxes under `scale(0.579)` at x / y phases of k/8 px and every band was fitted
(scratch tool: a page of absolutely placed boxes, coverage of `#b447ff` read back per band).

- Outer edges: the border box snapped to whole CSS px in the scaled space, then scaled: what KN
  already draws (0.03 per pixel).
- Inner edges, radius 0 / 2 / 3 / 5 / 8 with a 4 px border: area coverage (0.03).
- Inner edges, radius = width (2/2, 4/4, 6/6): a hard edge. A pixel whose centre is inside the
  padding box gets no border, the others keep their area coverage (0.03; area coverage misses by up
  to 0.52). This is Skia's stroked circular rrect with an inner radius of exactly 0.

Applied to the capture, this rule gives every inner edge above: Equip right starts at 763.00
(KN's 762.72 pixel has its centre inside), the bottom at 632.0, and the others keep their partial
pixel. The two outer edges still 0.21 px off (top, Equip left) are placement, not width.

**Fixed**
- `gl_backend.c` `border_hard_inner_edge` (uniform width, style, colour and radius, radius = width)
  flags the border, and `html_ui.frag` mode 6 then drops the coverage of a pixel whose centre is
  inside the padding box. Test: `html_gl_backend_test` row 0 (radius = width cleared, square and
  radius < width keep 0.2).
- `spin-item` buttons 5.62 -> 2.74 %, `-body` 7.29 -> 4.57, `-legendary` 6.29 -> 3.59, `-melee`
  7.66 -> 4.57: all four pass (max 5.5). Also lower: `spin-starter` / `spin-heroic` info
  2.23 / 2.53 -> 1.53 / 1.64, `ingame-pause-esc` class-card 0.63 -> 0.34, the hub profile cards
  0.17 -> 0.03, `menu-mobile` loadout 2.57 -> 2.10, `main-menu-nowplaying-long` class-card
  2.93 -> 2.61.
- `kn-map-vote` mapVote (0.00 -> 2.94 %, max 2): its reference is KN-own (2026-11-19) and drew the
  Tip button's 4px / radius 4px border with KN's old inner edge. The reference's 296 pixels where the
  new render differs from the old one were moved by exactly that difference (its other pixels,
  captured on the desktop, are kept); 0.00 again.
- Moved the other way, still passing: `kn-store-hats-v10` / `-melee-v10` picker 0.00 / 0.61 ->
  0.92 / 1.53 (KN-own references, same buttons), `kn-om-*` om-footer 2.12 -> 2.22,
  `customize-secondary` content 1.67 -> 1.85.

### Main menu and phone menu failures (2026-11-23)

Baseline: origin/main 5f2e9442 with the Skin Manager work, every scenario with a regions file, one
run on the devbox's llvmpipe: 767 passed / 47 failed / 82 skipped of 896 regions. The menu
failures were `main-menu` logo and click-to-play, `menu-mobile` header-right and logo,
`menu-mobile-portrait` logo.

**Fixed**
- **`main-menu` click-to-play** (25.86 -> 4.70 %, max 25 -> 8): not glyph advances. `#instructions`
  runs `grow` (0.8 s alternate, 44 px / opacity 0.3 to 36 px / 1). This reference caught it mid-way:
  ink 536..740 at peak 184 over the 17 background, about 37.4 px at opacity 0.88. Every other
  `main-menu-*` reference has it settled (36 px, peak 207, ink 540..737), which KN draws to 1 px.
  `anim_time` 0.45 is that phase, as `spin-item*`'s 0.05 is theirs; action-cards also goes
  0.79 -> 0.03 % and no other region of the scenario moves.

**Reference disagreement, not changed**
- **`#mainLogo`** (`main-menu` 6.08, `menu-mobile` 4.65, `menu-mobile-portrait` 5.56 %): the
  references need different mip sampling for the same image at a similar scale. Sampling an `<img>`
  trilinearly (the two levels around the exact lod blended, as Skia's linear mipmap mode does)
  passes all three (1.88 / 1.45 / 2.53 %) and the `hub-*` badges (1.84 -> 1.31 %), but
  `ingame-pause-esc` logo, the same `#mainLogo`, goes 3.80 -> 12.00 %, `ingame-mobile-portrait` timer
  fails (4.46 -> 6.64) and seven KN-own references fail (`kn-lb-grid`, `kn-menu-official-*`,
  `kn-ranked-found`, `kn-turf-*`, `kn-store-kio-eterno`). Over the whole set: 8 regions from fail
  to pass and 8 from pass to fail, so the floored level (`html_ui.frag` `sample_image`) stays.
  What made Chrome sample the two captures differently is not established (same capture tool for
  `menu-mobile` and `ingame-pause-esc`), so there is no rule to port yet.
- **`menu-mobile` header-right** (1.52 %, max 1): the text sits where Chrome's does (column profile
  best at shift 0, same rows 9..17). KN puts more coverage on the glyphs' top and bottom rows (row 8:
  38 vs 17, row 17: 106 vs 60, summed over the region), the vertical anti-aliasing of light hinting
  at the phone menu's fractional scale. Not changed.

**Death screen crosshair (checked, not ported)**: `ingame-death-self` and four of the seven
`seq-death` frames show a crosshair, `ingame-death` (killed by a bot) does not. v10.0.7 draws none
while dead: the overlay's `render` clears the canvas every frame and draws the crosshair only when
`deathUIHolder` is hidden (`IIiìíìi`, deob-10.0.7.js:162237, 162878; the same test in v10.0.2,
game.js:159839), and the death handler zeroes its opacity (`toggleGameUI(false)`, :163461). The
crosshair in those captures is a stale overlay frame (it flickers across the `seq-death` frames), so
KN keeps hiding it; no region covers the centre.

### Strike text, the static reference and flex shrink (2026-11-23)

Baseline: this branch on origin/main 9a7afde6 with the round 4 Skin Manager work, all 256 scenarios
with a regions file as eight shards on the devbox's llvmpipe: **801 passed / 53 failed / 89 skipped**
of 943 regions. The 36 failing scenarios again at `UIDIFF_FUZZ_SCALE=1.67` (fuzz +4..8): only five
regions still fail; the other 48 are colour drift. The five: `boot-curtain-350` instructions (KN-own
reference, see round 3), `strike/class-selection` selected-card, and `strike/item-selection` items-row,
default-square and title. The others on the list for this pass (`kn-servers-official` top-groups,
`menu-host-type`, `kn-ranked-sort` rs-arrow, `kn-tip-spin` cost, `ranked-shop-flying`,
`seq-end-collect`) already pass after the earlier triage.

After, on origin/main e2474665 (which has the round 4 work) with the fixes below: **806 / 48 / 89**, no
region from pass to fail; the 48 are the baseline's colour drift and `boot-curtain-350`. One earlier
full run (before the `v10_rewards.c` fix) had 803 / 51: `login-2fa`'s three regions failed (form-header
15.2 -> 47.9 %), that frame having the login form without its 2FA form. It was the only run of that
scenario whose log shows krunker.io's guest profile answered mid-render; the same binary passed it alone
and in a 4-shard run. `v10_rewards.c` no longer asks for it during a render (a render never reaches the
network, as `remote_images.c`), and no scenario log of the final run shows the request.

**Fixed**
- **GPOS kerning** (`text.c`, `strike/class-selection` class-list 2.29 -> 0.27 %, selected-card
  4.96 -> 1.49 %). FreeType's `FT_Get_Kerning` reads only the legacy `kern` table; ChakraPetch (Strike's
  font) has GPOS pair adjustments only, which Chrome applies, so KN's lines were wider and the diff
  started exactly at the kerned pairs (`kb`, `nt`, `at`, `ra`: -10 / -10 / -10 / -25 per 1000 em).
  `gpos_load` collects the `kern` feature's PairPos lookups (type 2, also through Extension type 9) and
  `gpos_kern` applies formats 1 and 2 (first subtable that covers the glyph decides), scaled like the
  legacy path. Test: `html_text_test` (A/V -2.72 px and k/b -0.32 px at 32 px, o/o 0).
- **The static reference's device scale** (`item-selection` title 2.56 -> 0.00 %). FRVR's own captures
  are Chrome under DevTools emulation, 640 x 360 CSS px at deviceScaleFactor 2, mobile
  (`capture-strike.js:20,67,339`), and KN lays the Strike page out the same way (`strike_ui.c`
  `apply_viewport`) and rounds text baselines in CSS px (`paint.c` `glyph_to_quad`). `static_ref.py`
  shot a 1280 x 720 window at `--force-device-scale-factor=1`, which rounds in device px: the title sat
  1 device px lower. A plain `--force-device-scale-factor=2` window gave the same pixels as scale 1;
  `Emulation.setDeviceMetricsOverride` (`tools/strike/static_ref_shot.js`) is what changes it. The
  reference and its kept page are regenerated with it.
- **Flex: a definite flex container's automatic minimum size** (`item-selection` items-row 8.99 ->
  0.58 %, default-square 6.85 -> 0.20 %). Not image sampling: `.item-selection__item-square` (120 px
  border box, 2 px border: 116 px of content) holds a 100 px image box and a 20 px row-flex label around
  a 16 px span. Chrome's rects (same emulation): image 96.67, label 19.33 at y 159.67, the 4 px shared
  100:20. KN took the content size suggestion from the laid-out content height, which for a flex
  container with a definite height is that height, so the label kept 20 and the image box took the
  whole 4 px (96.0, the banner 0.67 px lower, the pixelated image on another pixel grid). A flex
  container now also reports the height its lines would give an auto height (`LayoutOutput.auto_h`;
  a column's items at their hypothetical sizes), which `content_auto_h` uses (CSS Flexbox 4.5). KN's
  boxes match Chrome's to the dump's 0.1 px. Test: `html_layout_test` `t_flex_column_definite_item_min`
  (fails before: image 100, label 20; a definite column item kept 40 and starved its sibling to 0).

**Left**: `boot-curtain-350` instructions (0.97 %, max 0.5, KN-own reference: nothing new);
`item-selection` back / home are skipped regions (2.53 / 2.16 %, max 2).

### Boot-curtain instructions use the Chromium frame (2026-11-24)

A full four-shard run of main 0e13cda9 at `UIDIFF_FUZZ_SCALE=1.67` gave **847 passed / 7 failed /
89 skipped** of 943 regions. Besides this boot region, the six failures were the KN-only Account
Connections and Editor AI panels, left to the separate scroller work.

`boot-curtain-350` was still a standalone KN-own baseline. Its `boot: true` re-armed an already
booted page and `after_ms` then captured #instructions' independent `grow` animation at a different
phase from the curtain: its text ink was at x 536..740 with mean coverage 185, while Chromium's real
INIT + 350 ms frame has x 534..742 and mean 168. The native `seq-boot-curtain` render at INIT + 350
ms has Chromium's same bounds (x 534..742, y 284..311) and mean 166; the old baseline was not a
parity oracle for the text.

The standalone scenario is now that real one-frame INIT sequence, with the checker's scene and the
existing Chromium `sequences/boot-curtain/350.png` reference. No reference image changed. At normal
fuzz, its scene is 0.000 % and instructions 0.893 % (text fuzz 12, max 4, as the grow sequence);
`seq-boot-curtain` is also 10 / 10, with the curtain crossing at 353.7 ms in both reference and render
(timing delta 0.0 ms). The four-shard run after the migration is **848 / 6 / 89**: this instructions
region is the only outcome that changed, and no region went from pass to fail.

### Padded scrollers clamp against clientHeight (2026-11-24)

`kn-editor-ai`'s empty text painted 16 px above its own layout rect. Chromium on the captured DOM had the same rect
(y 288) and `scrollTop = 0`; KN had `scrollTop = 16` even though scrollHeight equalled clientHeight (551.75).
`layout_finish_scroll` subtracted `box_content_h`, excluding 12 + 4 px of padding, where Chrome's scroll range uses
clientHeight. It now uses the padding box minus the scrollbar on each axis. `html_layout_test` checks both axes on a
fitting padded scroller and the exact 20 px range of an overflowing one. The AI panel's four former failures are
0.000 % against the browser-verified, intentionally updated KN baseline; Account Connections is separately fixed by
pinning its loaded, unconfigured Discord response (2.148 / 1.502 %, both pass at normal fuzz).

### The login card's autofocus ring (2026-11-24)

v10's login card focuses the Email field when it opens (`v10_login.c` `render_with`). The Chromium capture
(`doc/ref/krunker-v10.0.2/ranked-guest.png`) has no focus ring. `state.blur` blurs `document.activeElement`
after the window's actions, and the Email field is now a region of `ranked-guest`, `login-header`,
`login-account` and `login-kio` (fuzz 12, max 1.5; a ring is several percent of that rect). The whole
`ranked-guest` card stays skipped: with the ring gone it is still 1.166 % at fuzz 6 and 0.548 % at fuzz 12,
the gradient bar's side-pixel coverage and the Login button's outer pixel. Those two pieces are already
scored by `login-header` and `login-account`. The threshold was not raised.

