# Plan: in-house HTML/CSS engine (pure C, no JS) replacing Ultralight

## Context

The native client renders the Krunker v3.1.1 menu/HUD (`ui/`) through Ultralight,
which is closed source. The user wants an open-source, JavaScript-free, pure-C
HTML+CSS engine with full visual and behavioural parity against the real client
(reference captures: `doc/ref/krunker-v3.1.1/`, spec: `doc/ui-feature-inventory.md`).

Alternatives were evaluated and rejected by the user: CEF (open source but a full
JS engine, 250-350 MB), Servo (Rust, immature C API), litehtml (BSD, but lacks
transforms/opacity/shadows/filters/animations/calc/grid/pointer-events/@font-face
and all form controls), RmlUi (needs porting the page to its own dialect).

Decision: **Lexbor** (Apache-2.0, pure C99) for HTML5 parsing, DOM, fragment
parsing, CSS tokenizer/parser and selector matching; everything above that
(cascade, computed style, layout, text, paint, animation, widgets, events) is
ours, in C11, rendering through the existing GL layer.

Verified on this machine (2026-10-10, Lexbor master `f4cbbcd`, version 3.1.0):
- `perl single.pl --port=posix html css selectors` → one 3.4 MB header; compiles
  as C11 in ~8 s; probe binary strips to 1.2 MB.
- Unsupported-but-known properties (`calc()`, `var()`, overflowing `z-index`) are
  kept as UNDEF declarations with their raw text; unknown properties
  (`transform`, `box-shadow`) are kept as name/value; `::after` and
  `::-webkit-scrollbar` rules come back as "bad style" rules with the raw selector
  and parsed declarations; `@keyframes` / `@font-face` bodies are preserved.
  So Lexbor is a sound parser and matcher, and the cascade must be ours.

## Architecture

```
client/include/html_ui.h           public API (init/fini/resize/update/render/input/menu-open/available)
client/include/html_dom.h          NEW DOM-ops + events + timers + storage API (native and web)
client/src/html/html_ui.c          dispatcher: menu-open flag, timer wheel, event queue -> handler table
client/src/html/engine/            NEW native engine (C11, Lexbor + FreeType + GL)
  engine.c      backend entry points, DOM-ops implementation, frame orchestration
  dom.c/.h      Lexbor wrapper: document, fragments (innerHTML), id map (first wins), mutation + invalidation, node->user = Box
  css_sheet.c/.h stylesheet loading: rule splitting, selector parse, pseudo-element tagging, @font-face/@keyframes, rule index
  css_values.c/.h parsers over token slices: lengths/calc/var, colors (incl. rgb() space syntax), transforms, shadows, gradients, filters, animation/transition shorthands, grid tracks
  css_props.h   X-macro property table (id, inherited, initial, shorthand expansion) for the 129 properties + vendor aliases
  css_cascade.c/.h match + cascade (UA/author/inline, specificity, order, !important) + inheritance + computed values + ::before/::after + restyle invalidation
  layout.c/.h   box tree; block/inline/line boxes/vertical-align/floats/inline-block; flex; grid subset; table/table-cell; absolute/relative/fixed/sticky; scroll containers + scrollbars; min-content; text-overflow; nowrap; rtl
  text.c/.h     FreeType faces, glyph atlas (2048^2 R8 shelf packer), kerning, synthetic bold/oblique, Material Icons ligature table, UAX#14 subset
  anim.c/.h     @keyframes, transitions (incl. `all`), timing functions (ease*, linear, cubic-bezier, steps), negative delays, alternate/infinite/fill
  paint.c/.h    display list, stacking contexts (CSS 2.1 App. E order), clips, per-context caching
  gl_backend.c/.h one program/one vertex format, SDF rounded rects/borders/analytic box-shadow, gradients (1D LUT), glyphs (atlas + Skia-like gamma LUT), images, FBO pool for opacity/filter groups, stencil for clip-path, scissor
  widgets.c/.h  range slider, checkbox switch, number, text/url/password (caret/selection/placeholder/maxlength), select popup, color swatch + picker
  events.c/.h   hit testing (pointer-events, transforms), hover/active/focus as attributes + restyle, inline on* handler parsing/dispatch with bubbling, scrolling, resize observers
  image.c/.h    stb_image decode (no flip), LINEAR/NEAREST per image-rendering, cache by URL, missing -> paint nothing
  util.h        arena, arrays, atoms, utf8, string builder
client/src/html/web_dom.c          Emscripten backend (phases 1-4): DOM ops via EM_ASM, browser renders
ui/js/kn-shim.js                   ~150 lines of web glue (handler globals -> ccall, data-kn-id, listeners, ResizeObserver, storage); replaces bridge.js/krunknative.js/hud.js/krunker-windows.js
ui/windows/*.html + windows.json   static window markup (generated once by tools/ui-import)
client/src/ui/menu.c               rewritten: menu state machine over DOM ops
client/src/ui/hud_html.c           rewritten: HUD over DOM ops
client/src/ui/ui_handlers.c        the inline-handler table (60 names)
client/src/ui/ui_windows.c         window loading + templating (marker splices replace the JS regexes)
client/src/ui/ui_settings.c        settings store (cJSON; native file / web localStorage) + defaults scraped from settings tabs
client/lib/lexbor/                 vendored amalgamation (lexbor.c/.h, LICENSE, README with commit + command)
tools/ui-import/                   woff2->ttf, krunker-windows.js->ui/windows, Material Icons liga table generator (python3 -I, fonttools+brotli)
tools/uidiff/                      scenarios + regions + ImageMagick per-region pixel diff
```

### Lexbor usage

| Need | Lexbor | Ours |
|---|---|---|
| HTML | `lxb_html_document_parse`, `lxb_html_document_parse_fragment` (context element = target, matches innerHTML) | id map, mutation hooks |
| DOM | `lxb_dom_*`, `lxb_dom_node_t.user` = Box/ElemData | — |
| CSS | syntax tokenizer + list-of-rules walker (qualified/at-rule/declaration callbacks); fallback: iterate `lxb_css_stylesheet_t` for selectors and re-tokenise declarations from byte ranges | one value parser (`css_values.c`) for sheets and inline `style=""`; `!important`; at-rule bodies |
| Selectors | `lxb_css_selectors_parse` on the prelude; `lxb_selectors_match_node`; `:hover/:active/:focus/:checked/:disabled` via attributes | pseudo-element stripping before parse (`::before/::after/::placeholder/::-webkit-scrollbar*/::-webkit-slider-thumb/::-webkit-*spin-button/::-webkit-color-swatch*`; `::-moz-range-thumb` dropped; `:visited` never matches), specificity from the AST, rule index (rightmost compound id > class > tag > universal) |

We do not consume Lexbor's typed property structs (calc/var collapse into one UNDEF bucket; known properties keep no raw text). One parser over token slices covers everything.

### DOM-ops API (`client/include/html_dom.h`, native + web)

`HtmlElem` (uint32, 0 = none; native index, web `data-kn-id`). Functions: `by_id` (first match), `query`, `query_all`, `parent/first_child/next/closest/child_count`, `create/append/insert_html/remove`, `set_html` (innerHTML), `set_text/get_text`, `set_attr/get_attr/remove_attr`, `set_class/has_class`, `set_display`, `set_style/get_style` (incl. `--kn-*`), `set_value/get_value`, `set_checked/get_checked`, `focus/blur/scroll_to_bottom`, `get_rect` (getBoundingClientRect), `offset_width/height`, `observe_resize`, `viewport(w,h,dpr)`, `set_handlers(table, fallback)`, `set_listener(id, type, fn)` (the three non-inline listeners: `#instructionHolder`/`#windowHolder` mousedown, document click on `<a>`, window keydown Escape), `html_timer_add/cancel`, `html_storage_get/set` (native `$XDG_CONFIG_HOME/krunknative/<key>.json`, web localStorage).

Inline `on*` attributes are parsed once per element by a tiny expression parser: `[return] callee(args)[; ...]`, callee ∈ `name`, `windows[N].name`, `SOUND.play`, `window.open`; args: string/number/bool/`this`/`this.value`/`this.checked`/`event`. Dispatch bubbles target→root; a handler returning 0 prevents default. Two bare expressions are special-cased.

### Engine entry points and frame

`html_ui_backend_init/resize/update/render/mouse_move/button/scroll/key/char`. Render pipeline: `anim_tick` → `cascade_restyle_dirty` → `layout_dirty` → `events_after_layout` (hover re-hit, resize observers, scroll clamp) → `paint_build` (dirty stacking contexts only) → `gl_submit`. Main thread only. Composite exactly as today (premultiplied, `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`, restore after); keep env knobs `KRUNKNATIVE_HTML_PROFILE/VERBOSE/DISPLAY_HZ`, 53 px per wheel click.

## C UI logic (replaces krunknative.js / hud.js)

- `menu.c` (~1,500 lines): `MenuUi` state; `menu_apply_open` (= applyMenuOpen), `menu_update_instructions` (4 states, error markup with `knRetry()`/`knHostOffline()`), top bar, class box, speech bubble (2,100 ms timer), `menu_show_window/toggle/close/rerender`, `menu_scale_base` (custom = 2 − scaleUI; 1700×900·custom design space; `%.3f`; transform/width/height on `#uiBase`; `uiScaling=false` clears; on init/resize/`scaleUI`/`uiScaling`), `menu_send_preview_rect` (rect of `#classPreviewCanvas` → `client_class_preview_set_rect` directly; the preview is the canvas's own bitmap, `html_dom_canvas_gl`), net/state glue kept (`menu_poll_connection`, `menu_try_play`, `client_set_menu_open`; `connect` → `menu_set_connect` + 6 s timer), account name commit (ASCII, trim, 14 chars).
- `ui_handlers.c` (~700): the 60-entry table (setSetting, delayExecute, showWindow, selectClass, playTick/SOUND.play no-ops, openURL/openTab/window.open blocked, host/join/invite/quickJoin/joinGame/createPrivateRoom, registerAcc/loginAcc → commit name, reset/export settings, playMap, selectHostMap, knHostMap, knRetry, knHostOffline, windows.0.changeTab/searchList/collapseFolder, windows.3/9.switchTab, bdayPlay, checkTerms, ~25 no-ops, and the 7 names undefined in Krunker's own JS). `h_set_setting` stores, applies to the game (`client_apply_setting`: scaleUI/uiScaling/sensitivity/fov/showFPS at least), syncs `slid_<key>`/`slid_input_<key>`.
- `hud_html.c` (~900): existing 20 Hz sampling kept; `hud_render_*` write the DOM directly (visibility ~35 writes, health bar innerHTML, ammo `background-size`, timer/class/scope/ping/loadout/scoreboard/crosshair), chat seed + `scroll_to_bottom`, kill feed (cap 8, fade 3.5 s, remove 4 s), hit marker stepped per frame.
- `ui_settings.c` (~300), `ui_windows.c` (~400): defaults scraped from `ui/windows/settings-tab-*.html` via detached fragment + the same selectors as `collectDefaults`; `windows.json` maps ids → files/templates.

## Data structures (summary)

`CssLength {px, pct, kind}` (calc(100% − 35px) = {−35, 100}); `CssColor`; `Mat2D`; `CssTransformOp[4]`; `CssShadow` (one box-shadow, up to 16 text-shadow layers); `CssGradient` (linear/radial, ≤8 stops); `CssBgLayer[2]`; `CssFilterOp[6]`; `CssTiming`; `CssAnimation[2]`; `CssTransition[2]`; `CssTrack[8]`; `CssCustomProp[12]`; `CssComputed` with every inventory property plus `hash_inherited/hash_non_inherited` for invalidation. `Box` (type, node, style, pe, geometry in CSS px floats, margins/borders/padding, scroll state, sticky offsets, transform, line boxes, text runs, dirty flags, cached display list per stacking context); `LineBox`, `InlineItem`, `TextRun`, `Glyph` (subpixel phase). `DlItem` ops: RECT, RRECT, BORDER, IMAGE, GRADIENT, GLYPHS, BOX_SHADOW, TEXT_SHADOW, PUSH/POP CLIP (rect/rrect/poly), PUSH/POP TRANSFORM, PUSH/POP GROUP (opacity/filter → FBO), SCROLL_OFFSET.

## GL rendering

One program, vertex = pos, uv, premultiplied rgba8, params vec4, mode; streaming VBO/IBO orphaned per frame (GLES3-safe). Modes: solid, rounded-rect SDF, image, glyph (R8 atlas + 256×8 gamma LUT), gradient (1D LUT texture), analytic blurred box-shadow (erf approximation; inset via inverted coverage), border SDF with dash/dot, group composite (opacity × 4×5 color matrix; drop-shadow = separable 7-tap blur into a pooled FBO). Per-vertex clip rect + radius for one level of rounded clipping; nested/transformed clips via scissor or stencil; `clip-path: polygon` via stencil. Transforms applied on CPU to vertices; glyphs rasterised at device size (atlas key = face, size·scale in 1/64 px, synth flags, ¼-px x phase). Draw calls break only on texture set / scissor / stencil / blend / FBO change (≈20–60 per menu frame). Images: no flip, LINEAR+CLAMP, NEAREST for `pixelated/crisp-edges`; cover/contain/object-fit/repeat round/position resolved in paint.

60 fps with `grow` on `#instructions`: layout invalidation stops at layout boundaries (absolutely positioned, definite size, overflow≠visible), so only `#instructions`' line box relayouts; paint re-records only its stacking context; the GL stream is rebuilt from cached lists (<0.2 ms). `scaleBob` is transform-only. Scrollbars painted as `BOX_SCROLLBAR` parts with Chrome's custom-scrollbar metrics (no buttons; thumb min = its `min-height`), styled via the `::-webkit-scrollbar*` pseudo rules; rtl puts the bar on the left.

## Build changes

1. Vendor `client/lib/lexbor/{lexbor.c,lexbor.h,LICENSE,README.md}` from the pinned commit (`single.pl --port=posix html css selectors`; add `style` only if needed); `add_library(lexbor STATIC)`, `-w` on that target, builds under emcc too.
2. `CMakeLists.txt`: drop `cmake/ultralight.cmake`; `option(KRUNKNATIVE_HTML_ENGINE ON)`; native ON → `engine/*.c` + `-DKRUNKNATIVE_HTML_ENGINE=1`, link `lexbor freetype`; OFF → `stub.c`; Emscripten → `web_dom.c` (phase 5: engine). Web link: export `_kn_ui_handler`, `_kn_ui_resize_observed`; `--preload-file ui/windows@/ui/windows`; drop `_krunknative_html_call`. New native target `ui_test` (engine + hidden GLFW window) for dumps/diffs/benchmarks.
3. Delete `cmake/ultralight.cmake`, `client/src/html/{ultralight.c,web.c,INTEGRATION.md}`, `third_party/` (+ .gitignore line), `ui/js/{bridge,krunknative,hud,krunker-windows}.js`; `ui/index.html` scripts → `kn-shim.js` + `loader.js` (phase 5: `loader.js` only). Rewrite `doc/html-ui-bridge.md` → `doc/html-ui-engine.md`.
4. Makefile: `HTML_UI` → `HTML_ENGINE`; remove `sdk*`; `lint` checks only `kn-shim.js`/`loader.js`; new `ui-import` (python3 -I tools/ui-import/import.py; outputs committed) and `uidiff` (builds `ui_test`, renders `tools/uidiff/scenarios/*.json` under Xvfb/llvmpipe, `compare -metric AE -fuzz` per region from `tools/uidiff/regions/*.json`, table + diff PNGs, exit 1 over threshold); `smoke-native` greps `[ui] handler setSetting`.
5. CI: remove Ultralight caching/staging/7z; Windows builds with `HTML_ENGINE=ON` (pure C); Linux adds imagemagick + xvfb and `make uidiff` (advisory until phase 2, required after phase 3); `make check` gains `uidiff` after phase 3.

## Phases (each ends with `make`, `make smoke`, `make uidiff`)

| Phase | Deliverable | Est. C lines |
|---|---|---:|
| 0 Foundations | Lexbor vendored (native + emcc); dom.c; css_sheet.c; css_values.c (lengths/calc/var/colors/keywords); css_props.h; css_cascade.c with UA sheet, inheritance, computed values, ::before/::after; `ui_test --dump-style` / `--check-style` against ~60 (selector, property, expected) triples read from `real-1280.dom.html`; `tools/ui-import` outputs committed; CMake/Makefile/CI switched, Ultralight removed, stub when OFF | ≈6,000 (+500 py) |
| 1 Static menu | layout.c block/inline/line boxes/floats/inline-block/positioned/min-content/nowrap; text.c; image.c; paint.c backgrounds/borders/radius/clips/text+text-shadow/stacking; gl_backend modes 0–4, 6; menu renders with `menu_apply_open(1)`; scenarios `menu-1280`, `menu-1920` | ≈7,500 |
| 2 Layout parity + state | flex, grid, table, sticky, scroll containers + scrollbars + rtl, text-overflow, transforms, opacity groups, box-shadow (incl. 4000 px hole), hover/active + restyle, mouse/wheel events, all 13 windows via showWindow; scenarios for every window | ≈6,000 |
| 3 Widgets + full logic | widgets.c; focus/keyboard; ui_handlers.c; ui_settings.c; hud_html.c rewrite; timers; class_preview via `get_rect`; web build on `web_dom.c` + `kn-shim.js` (old JS deleted); in-game scenarios | ≈5,500 |
| 4 Motion + perf | anim.c (all 13 keyframes incl. from-only `grow`, steps(36), cubic-bezier, negative delays, alternate/infinite/fill; transitions incl. `all`); filters; clip-path; visibility; rtl edge cases; layout boundaries + paint caching; profiling; atlas eviction; perf gate `ui_test --bench menu-1280` < 2 ms/frame restyle+layout+paint on llvmpipe | ≈3,000 |
| 5 (optional) Web on the same engine | engine under emcc (GLES3), `web_dom.c`/`kn-shim.js` removed, canvas-only page | ≈500 |

Total ≈ 28k lines of C (engine ≈ 22k, UI logic ≈ 4k, tests/tools ≈ 2k) + ~600 lines Python/sh.

## Verification and risks

- **Text vs Chrome/Skia (highest risk).** FreeType unhinted, device-size rasterisation with ¼-px x phases baked into outlines, Skia-style gamma/contrast preblend LUT in the shader, synthetic bold (`FT_Outline_EmboldenXY`) and oblique (shear). Knobs `KRUNKNATIVE_HTML_HINTING/TEXT_GAMMA/SUBPX` so `make uidiff` can sweep; text regions fuzz 10–15 % / ≤3 %, non-text 4 % / ≤0.5 %. Fallback font path for missing glyphs; U+200E zero-width.
- **Chrome UA stylesheet subset** (engine/ua.css): block/inline defaults, `a` link styling, `b/i`, replaced-element intrinsic sizes (canvas/video 300×150, img 0×0 when missing, no broken icon without alt), Chrome's refreshed form-control defaults (input 13.333px Arial, border/padding/colors; checkbox 13×13; range 129×20 with `::-webkit-slider-thumb` when `appearance:none`; color 50×27 + swatch; select menulist with chevron; `::placeholder #757575`), `ul` margins/padding, `[hidden]`. Each cross-checked against the window captures.
- **Scrollbar metrics** verified on `win-host.png`; sticky offsets against the nearest scroller.
- **int32 z-index clamp**, stable sort by document order. **Duplicate ids**: first in document order; rescan on removal.
- **Malformed markup** handled by Lexbor's HTML5 tree construction; unknown input types → text; attribute entities decoded by Lexbor.
- **Lexbor matcher quirks** confirmed in phase 0 (`ui_test --check-selectors`): attribute-backed state pseudo-classes, `:not(:placeholder-shown)`, `:nth-child`, `+` across text nodes, case-insensitive font-family, per-selector specificity from match results.
- **Missing assets** → paint nothing (as Chrome). **`sprayAnm`** ignored; `grow` with only `from` uses computed values as `to`.
- **Performance**: host window `set_html` (2,111 elements) restyle < 30 ms via the rule index; FBO groups rare; atlas keys rounded to ¼ px so `grow`'s font-size sweep stays in one atlas.
- **Input parity**: wheel 53 px/click, slider pointer capture, caret blink 500 ms, selection only in inputs, Escape blurs a focused input before toggling the menu.
- **Deliberately not implemented** (document): `title` tooltips, `:visited`, `@media`, text selection outside inputs, IME, `<video>` playback, `will-change`, `loading=lazy`, non-inline `<script>`.

## Critical files

- `client/src/html/html_ui_backend.h`, `client/include/html_ui.h`, new `client/include/html_dom.h` — contracts.
- `client/src/html/ultralight.c` — compositing, input mapping and pacing behaviour to preserve before deletion.
- `client/src/ui/menu.c`, `client/src/ui/hud_html.c` (+ `ui/js/krunknative.js`, `ui/js/hud.js` as port sources).
- `CMakeLists.txt`, `Makefile`, `.github/workflows/build.yml`.
- `doc/ui-feature-inventory.md`, `doc/ref/krunker-v3.1.1/` — the spec and the acceptance references.
