# Developer console invisible after the round end

Symptom: the console is open (` toggles it, the input takes keys, clicks hit it) but nothing
of it draws, after the defeat / victory end sequence.

## Root cause

`client/src/html/engine/gl_backend.c`, the group framebuffer pool (`fbo_acquire`).

- The pool holds `GLB_MAX_FBOS` (24) entries and never resizes or frees them. On origin/main
  (`gl_backend.c:581`) a request no free entry fits returned -1 for good once the pool was full.
- An opacity group that gets -1 skips its whole subtree (origin/main `gl_backend.c:1718-1727`:
  `f->skip = 1; b->skip_depth = 1`). Nothing is logged.
- The console's `.modal` has `opacity: 0.95` (`ui/css/devconsole.css:30`), so it is an opacity
  group sized to its box: 758x294 px becomes 1024x512 (pot). After the end, the pool held 24
  free entries of other sizes and none was 1024x512, so the modal was skipped. The probe still
  reports it shown, boxed and topmost, because the hit test does not depend on the GL draw.

Reproduced by `client/tests/console_draw_sweep_e2e.py dead` (the death menu, `con kill`, then
the end sequence): every open frame drew no console (grey share 0.00). With a temporary log,
the pool printed `FBO POOL FULL 1024x512 in_use=0` on each of those frames.

Fix: when the pool is full, `fbo_acquire` frees the smallest free entry and reallocates it at
the requested size (`fbo_gl_free` / `fbo_gl_alloc`). It returns -1 only when every entry is in
use, which is a group deeper than the pool and is not reached by the sweep.

## Checked and ruled out (this failure)

- Top-layer routing (`paint.c` `flatten_context`): with the console open its box was present
  and 35 records went to `top_dl`, which `html_ui_render_top` submitted.
- Overlay opacity / z-index: 1.000 on the overlay and its ancestors, z-index 2147483647.
- End-screen show/hide (`hud_ui.c`): they touch `endUI`, `endInfo`, `endMidHolder`,
  `victorySub`, `uiBase`'s class and `knVicTitle`, never the console overlay.
- Client draws after the page (confetti, viewer embed) and their GL state: `glb_begin` resets
  blend, colour mask, scissor, stencil, depth and the target each frame.
- Frame target (`frame_target.c`): off on native builds unless `KRUNKNATIVE_FRAME_TARGET` is set.

Not reproduced: box-recycling cache hits (`paint.c` `record_if_needed`, `layout_new_box`) and
stale incremental paint flags. They are not needed to explain this bug, and no failing frame
pointed at them.

## Test

`client/tests/console_draw_sweep_e2e.py BUILD_DIR [scenario ...]`: headless offline Kanji
client, one scenario per opening point (`hold`, `sweep`, `sweep2`, `menu`, `dead`, `after`).
The toggle runs every 50 ms through the end, the title, the results, the next round and the
respawn. Each frame's probe (`input_script` `frames` log line) says whether the console is open;
open frames must show its grey panel, and must match the first open frame.

The grey threshold is 0.7 (was 0.85). The panel is 0.95 opacity, so a bright scene behind it
lifts a few sampled points: a correctly drawn console read 0.84 in `sweep2` (f_0185).

## Results

- Before (origin/main 09491c444, pre-fix `gl_backend.c`, same test): 21/22 checks passed.
  `dead` failed every open frame (984), grey share 0.01 (0.00 in the first run on 1958edd96).
- After (the fix, rebased on 09491c444): 22/22 checks passed; every open frame of all six
  scenarios draws the console.
