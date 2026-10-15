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

## Left, in order of size

- Per-draw uniform traffic: `basic_material_update_uniforms` sets ~40 uniforms per draw through
  the cache; the lighting/fog/camera block is the same for every draw of a program and could be
  set once per program per frame (or moved to a UBO).
- GPU time: the frame is GPU-bound on Kanji at 1280x720 with default settings. A GPU timer-query
  breakdown (shadow pass, scene, post, HTML page) is the next step before changing render passes.
- Start-up: `map_catalog_json` walks cJSON arrays by index (`cJSON_GetArrayItem` is O(n)); a
  linked-list walk would make it linear.
