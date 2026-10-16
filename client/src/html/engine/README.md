# Krunker Box Office HTML/CSS engine (pure C11, no JavaScript)

The only HTML UI backend of the native client (built by default, `KRUNKNATIVE_HTML_ENGINE=ON`;
OFF builds client/src/html/stub.c instead). It renders ui/index.html and ignores `<script>`:
the page is driven by the C UI logic in client/src/ui through client/include/html_dom.h.

Contract: doc/html-ui-engine.md. Plan: doc/html-engine-plan.md. Spec: doc/ui-feature-inventory.md.
References: doc/ref/krunker-v3.1.1/.

## Modules and header contracts

| Module | Files | Public header | Notes |
|---|---|---|---|
| util | util.c/.h | util.h | arena, growable arrays, atoms, utf8, string builder, hashing |
| css values | css_values.c/.h, css_types.h, css_props.h | css_types.h, css_values.h | ALL Css* value structs incl. CssComputed live in css_types.h; tokenizer + parsers over token slices, computed-value apply/inherit/hash, CSS zoom scaling (css_computed_zoom; the cascade computes unzoomed then applies the effective zoom) |
| dom | dom.c/.h | dom.h | Lexbor wrapper: document, fragments (innerHTML), id map, attributes/classes/inline style, pseudo-class state, style-dirty bits |
| stylesheets + cascade | css_sheet.c/.h, css_cascade.c/.h, ua_css.h | css_sheet.h, css_cascade.h | rules/selectors/rule index, @font-face, @keyframes; UA + author cascade -> CssComputed per element (+ ::before/::after, scrollbar/control pseudo-elements) |
| text | text.c/.h, material_icons_liga.h | text.h | FreeType faces, font matching, shaping-lite, glyph atlas (GL R8), synthetic bold/oblique, Material Icons ligatures, line breaking, gamma LUT |
| image | image.c/.h | image.h | stb_image decode (no flip, premultiplied; a still `.webp` through webp.c over client/lib/simplewebp, an animated one from the importer's `.webp.png` sidecar), cache by URL (hashed), LINEAR/NEAREST textures, background/object-fit geometry helpers; file rasters are lazy (header-only for layout, decoded on first draw under the engine's per-frame budget, LRU-evicted past IMAGE_CACHE_BUDGET_BYTES) |
| svg | svg.c/.h (+ client/lib/nanosvg) | svg.h | SVG images for image.c: viewBox / preserveAspectRatio mapping, rasterised per drawn device size and sub-pixel offset (gl_backend's image resolver) |
| video | video.c/.h | video.h | `<video>` without a decoder: plays the importer's `<file>.knv` sidecar (JPEG frames, tools/ui-import/video_frames.py) on the engine clock, one GL texture per element updated in place (autoplay / loop / poster; natural size from the header) |
| layout | layout_build.c (box tree, incremental rebuilds, zoomed natural sizes), layout_block.c (block/floats/positioned), layout_inline.c (lines), layout_flex.c, layout_grid.c, layout_table.c, layout_query.c (hit test, rects, stacking order), layout_internal.h | layout.h | consumes dom.h + computed styles + text.h; `layout_restyled` / `layout_children_changed` for incremental updates |
| paint | paint.c/.h, paint_types.h | paint.h, paint_types.h | display list in CSS 2.1 App. E order, per-stacking-context caches (content-hashed), clips/transforms/groups/scrollbars |
| gl backend | gl_backend.c/.h, assets/shaders/html_ui.{vert,frag} | gl_backend.h | submits DisplayLists through one program; premultiplied composite; restores the client's GL state; culls primitives outside the scissor / target; resolves lazy images per draw |
| anim | anim.c/.h | anim.h | @keyframes + transitions, timing functions, interpolation; output written over the computed style by engine.c |
| events | events.c/.h | events.h | hit testing via layout, hover/active/focus state, inline `on*` handler parsing + bubbling dispatch, wheel scrolling, resize observers |
| selection | selection.c/.h | selection.h | page text selection outside form controls (mousedown's default action): drag, double/triple click word/block, shift+click, Ctrl/Cmd+A of the focused region, Ctrl/Cmd+C to the GLFW clipboard, Escape/click-away; honours user-select:none, ::selection colours (paint.c highlight via PaintHost.selection_range) |
| widgets | widgets.c/.h | widgets.h | form controls: text/number/password, textarea, checkbox, radio, range (+ ::-webkit-slider-thumb / -runnable-track), color, select (state, Chrome 152 painting, keyboard/mouse); the code view of a textarea (gutter, colour spans, line marks, no wrap) and text editing from C (selection, replace, caret rect) |
| engine | engine.c/.h | client/src/html/html_ui_backend.h, engine.h | backend entry points (init/update/render/input), frame orchestration (anim -> restyle -> incremental layout -> observers -> paint -> submit), host adapters for layout/paint/events/widgets |
| DOM ops | dom_ops.c | client/include/html_dom.h | native html_dom_* (handlers/timers/storage are in client/src/html/html_dom_core.c) |

Tools: `client/src/html/ui_test.c` (`make ui-test ARGS=help`): render-page (offscreen PNG,
`--menu-preset`), bench, bench-dynamic (hover sweep + 20 Hz HUD writes), dump-boxes,
lexbor-probe. Whole-client renders and pixel diffs: `make uidiff` (`client_bin
--render-scenario`, tools/uidiff). Per-module standalone tests live in `tests/`. Debug env
knobs: KRUNKNATIVE_HTML_PROFILE, KRUNKNATIVE_HTML_VERBOSE, KRUNKNATIVE_HTML_DISPLAY_HZ,
KRUNKNATIVE_HTML_DUMP_DL (engine.c), KRUNKNATIVE_HTML_IMAGE_LOG (image.c) plus the text.h knobs (KRUNKNATIVE_HTML_HINTING,
KRUNKNATIVE_HTML_TEXT_GAMMA, KRUNKNATIVE_HTML_OBLIQUE_SKEW, KRUNKNATIVE_HTML_FAKE_BOLD_ADVANCE,
KRUNKNATIVE_HTML_FALLBACK_FONT); all listed in doc/html-ui-engine.md.

Rules: C11 only, follow AGENTS.md style (small structs/functions); must build on Linux,
Windows (MinGW) and macOS. Every module has a standalone test in `tests/`, built from a
throwaway /tmp CMake project. They read Krunker v10.0.2's page in `ui/`; the cascade's
(test_cascade.c: the v7.1.4 menu DOM against Chrome's computed styles, tests/data/v7-menu-1280.*)
and layout's Part 2 (the same menu against Chrome's rects) were retired with the v7.1.4 page;
`html_runtime_sheet_test` (a CMake test) runs the cascade over the v10 page's sheets.
Lexbor is used only through dom.c/css_sheet.c (parsing, DOM, selector matching).
GL goes through `client/include/gl.h` (GL 4.5 core natively, GLES3 on the web).
