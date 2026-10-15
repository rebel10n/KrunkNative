# Client performance audit (2026-10-15)

Native client, headless on the RTX 2070 SUPER (EGL, NVIDIA 595), 1280x720, offline, default
settings, uncapped (`KRUNKNATIVE_FRAME_CAP=0 KRUNKNATIVE_VSYNC=0`), a 15 s input script (play,
three look turns, quit). CPU: `perf record -F 999/1999 -g --call-graph fp` on a RelWithDebInfo
build with `-fno-omit-frame-pointer`. The A/B runs use `KRUNKNATIVE_FIXED_DELTA=16.667`, so both
binaries run the same number of game frames and the sample counts compare directly.

## Where a frame goes

Almost all samples are on the main thread; the offline server, net and audio threads are
under 1%. Before the fixes, of the main thread's samples:

| Share (Kanji / Sandstorm) | What |
|---|---|
| 12-15% | NVIDIA driver spinning in `clock_gettime`: the headless pbuffer fence wait (at most two frames in flight). This is the GPU-bound part: the GPU sets the frame rate (~500-700 fps on Kanji, 450-1000 on Sandstorm, on a desktop GPU shared with other work) |
| 28% / 24% | `scene_render` (inclusive; includes driver time under the GL calls) |
| 11.5% / 7.1% | `mesh_update_transform_matrix`: every mesh's world matrix rebuilt every frame (5 4x4 products and 12 trig calls per transform node), map geometry included, which never moves |
| 4.1% / 2.4% | `qsort` of every mesh (opaque ones included) with a comparator that only orders the transparent ones; glibc's merge sort mallocs each call |
| 6.3% / 7.6% | `basic_material_update_uniforms` and the `gl_uniform_*` cache (per-draw uniform lookups and compares; most uploads are skipped as unchanged) |
| 1.7% / 1.5% | `ui_mods_tick` -> `poll_switch`: two document-wide selector queries every frame for the Settings window's developer console / force offline switches, with no window open |
| 3.7% (Sandstorm) | `hud_objectives_render` -> `html_dom_is_shown` -> `engine_flush_layout`: the frame's style/layout flush happens here instead of in the page render; moved, not extra, work |
| ~2% of a 15 s run | `map_catalog_load_official` -> `map_catalog_json`: cJSON array walks at start-up (one time, ~0.3 s) |

## Fixed

1. **World matrices are cached** (`client/src/mesh.c`, `Mesh._xf_cache` in `client.h`): each mesh
   keeps a by-value snapshot of its transform chain (node address, parent, order, position,
   rotation, scale). An unchanged chain keeps its matrix; any change, a re-parent or a copied
   mesh rebuilds it with the same arithmetic as before (identical matrices).
2. **Only transparent meshes are sorted** (`client/src/scene.c`): one pass splits the visible meshes
   into opaque (scene order, as the stable sort left them) and transparent (sorted back to front
   on the camera-space depth, computed as one dot product instead of a full 4x4 product; equal
   depths in scene order). Hidden meshes are left out of the draw list.
3. **The Settings switches are polled only with a menu window open** (`client/src/ui/ui_mods.c`).

A/B, same frames (samples at 1999 Hz):

| | Kanji before | after | Sandstorm before | after |
|---|---|---|---|---|
| `mesh_update_transform_matrix` | 3447 | 652 (-81%) | 2751 | 561 (-80%) |
| `qsort` | 1658 | 185 (-89%) | 805 | 75 (-91%) |
| `poll_switch` | 679 | 0 | 612 | 0 |
| `client_menu_tick` | 1538 | 893 | 1426 | 660 |
| `client_tick_game` | 11694 | 11425 | 9801 | 6782 (-31%) |

On Kanji the saved CPU time turns into driver wait (the GPU is the limit there); on Sandstorm the
game frame's CPU drops by a third. World captures of both maps from fixed cameras match the old
build pixel for pixel, except an ad billboard that picks a random image.

## Round 2: GPU per pass, per-draw uniforms, start-up

### Per-pass timing

`KRUNKNATIVE_GPU_TIMING=1` (client/src/gpu_timer.c) prints once a second the average GPU time
(GL_TIMESTAMP queries, read back 3 frames late so nothing stalls; desktop GL only, GL ES has no
timer queries) and CPU time of each pass: clear, pre (scene target clear, static shadow map,
ocean mirror), sky, world, vfx, viewmodel, post (scene target upscale), hud, preview, page.

Kanji / Sandstorm, 1280x720, default settings (resolution 0.6: a 768x432 scene target), best
second of each run (the RTX 2070 SUPER is shared with the desktop, OBS, Sunshine and other
agents' clients, so single seconds vary by 2x; medians tell the same story):

| GPU ms | sky | world | viewmodel | post | page (HTML HUD) | total |
|---|---|---|---|---|---|---|
| Kanji 0.6 | 0.008 | 0.24-0.28 | 0.011 | 0.015 | 0.32-0.50 | 0.81 |
| Kanji 1.0 | 0.011 | 0.29-0.32 | 0.02 | - | 0.40-0.47 | 0.73-0.95 |
| Sandstorm 0.6 | 0.008 | 0.10-0.14 | 0.014 | 0.015 | 0.34 | 0.51-0.62 |

- **The GPU is not the limit.** The whole frame is under 1 ms of GPU time. Without other GPU
  clients the frame is CPU-bound; the driver spin in round 1's profile was the headless fence wait
  while the GPU worked on other processes' frames.
- **Shadows:** the sun shadow map is static (rendered once after a map load, 1024 px, 4096 with
  highResShad), so it costs nothing per frame; the receiver lookup is part of "world".
- **Fog** is a few ALU ops in basic.frag; **post** (the scene target's upscale) is 0.015 ms; the
  **sky** dome 0.01 ms. **Overdraw**: the world pass at 768x432 is 0.1-0.3 ms; moving to full
  resolution adds about 0.05 ms, so fill rate is not a factor at these settings.
- The page interval (the HTML HUD's composite, ~30 draws, two small opacity groups) is the largest
  GPU item in most seconds, but it also absorbs time the GPU spends on other contexts between our
  submits (hiding the page moves that time into "world"). Nothing in its display list is large.

### Fixed

1. **Scene-wide basic-material uniforms once per program** (`client/src/materials/basic.c`): the
   sun, ambient, lighting mode, the light arrays (rebuilt with trigonometry for every draw before)
   and the clip plane are keyed per program on their inputs (a light / clip generation counter,
   the lighting values, the ruleset light units, the uniform cache's relink epoch); an unchanged
   key skips them. Fog and tone mapping stay per draw (other files write those names). Hit rate
   in game: >99.9% (2 misses in 1.2 M draws on Sandstorm).
2. **Camera uniforms once per program per scene pass** (`client/src/scene.c`).
3. **Start-up map walk** (`shared/src/map_catalog.c`, `shared/src/map.c`, `client/src/ambient.c`):
   sanitising a v7 map looked every pooled size up with `cJSON_GetArraySize` / `cJSON_GetArrayItem`
   on the `xyz` pool (both walk the linked list from its head: quadratic in the object count);
   the pool is now read into an array once. The map parser's object and colour loops and the AO
   collector walk their lists instead of indexing them.

A/B on the same game frames (fixed delta, samples at 1999 Hz):

| | Kanji before | after | Sandstorm before | after |
|---|---|---|---|---|
| map catalogue load (start-up) | 7843 (3.9 s) | 1066 (0.5 s) | 7848 | 1026 |
| `basic_material_update_uniforms` | 4066 | 1928 (-53%) | 2571 | 2678 (within run-to-run noise) |
| `scene_render` | 8395 | 5490 (-35%) | 6166 | 6905 (within run-to-run noise) |

Frame time, NVIDIA, best second, 1280x720 uncapped: Kanji 0.6 0.877 -> 0.872 ms, Kanji 1.0
0.961 -> 0.739 ms, Sandstorm 0.6 0.645 -> 0.530 ms, Sandstorm 1.0 0.628 -> 0.686 ms. The noise on
the shared GPU is larger than the change; the CPU sample counts above are the reliable measure.
World captures of Kanji and Sandstorm from the 4 fixed cameras match the old build pixel for pixel
except the ad billboard (a random image).

## Left

- The HTML HUD composite: measure on an idle GPU before changing glb_submit.
- `basic_material_update_uniforms` still makes ~30 cached per-material uniform calls per draw;
  grouping draws by material (most map objects have their own colour) would not remove them.
