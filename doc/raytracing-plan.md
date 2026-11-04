# Plan: ray tracing in Krunker Box Office

Status: historical. Implemented 2026-11-02; the as-built reference is doc/raytracing.md (contract:
client/include/rt.h). Written against the renderer as of commit efbf84d9.

## What "ray tracing" can mean here

The renderer is a forward OpenGL pipeline (`client/src/scene.c`, `assets/shaders/basic.frag`)
that must stay inside the subset shared by desktop OpenGL 4.5 and OpenGL ES 3.0
(`client/include/gl.h`, AGENTS.md "Web Build Rules"): ANGLE/Metal on macOS and WebGL2 on the web
run the same code. That subset has **no compute shaders, no SSBOs, no image load/store and no
ray-tracing extension**. OpenGL has no RT-core extension on any vendor; the RT cores of the
user's RTX 2070 SUPER are reachable only through Vulkan (`VK_KHR_ray_query`) or DXR/Metal,
i.e. a second graphics backend or GL/Vulkan interop (`GL_EXT_memory_object` + `GL_EXT_semaphore`,
NVIDIA/AMD desktop only). That is a large new dependency (Vulkan loader, SPIR-V shader build,
a second resource model) which AGENTS.md rules out unless the subsystem already adopted one.

Decision for this plan: **GPU ray tracing in GLSL fragment shaders over a BVH stored in float
textures.** It runs on every platform the client already supports, needs no new library, and
plugs into the existing material/pass structure. A hardware (Vulkan ray-query) accelerator is
listed as a later, desktop-only, optional phase once the portable path proves the visuals.

Every ray-traced effect is **off by default**. The default frame stays pixel-identical to today
(Krunker parity, user 2026-10-29: no KN-only player-facing additions). Ray tracing is an
explicit opt-in on the Render tab, next to the other Box Office-only rows (vsync, Window Mode in
`client/src/ui/ui_render_settings.c`).

## What the scene looks like (why a custom BVH is cheap)

| Map | objects | prefab 0 (cube) | other |
|---|---|---|---|
| Sandstorm | 1270 | 895 | planes 152, ramps 68/66, cylinders 54, OBJ models ~30 |
| Kanji | 1746 | 1680 | models 27, trees/other < 40 |
| Kanji (v3 layout) | 2001 | 1760 | 133 custom assets |

- Cubes, planes and ramps share one unit geometry each (`g_cube_geometry`, `g_plane_geometry`,
  `g_ramp_geometry`) scaled and rotated by the `Mesh` transform. They get **analytic
  intersectors** in the shader (ray vs unit box / unit quad / unit wedge in local space), not
  triangles: one instance record per object, no triangle BLAS.
- Cylinders, spheres, OBJ models (`load_obj_model`), glTF (`gltf.c`), terrain (`terrain_mesh.c`),
  the AO planes (`ambient.c`) and `map_post.c` builds are triangles. OBJ geometry keeps only
  `tri_indices` on the CPU today (`Geometry.tri_indices`); positions must be retained too.
- Players are hierarchies of cube `Mesh`es (`scene_add_player_mesh`) or a glTF class model: all
  instances again, so a per-frame top-level rebuild covers them with no skinning work.
- Lighting today: one sun (`g_render_lighting.light_direction`), ambient, fog, a static
  1024/4096 hard shadow map (`shadow.c`, BasicShadowMap), up to 16 forward lights
  (`BasicLight`), fake AO strips (`ambient.c`), an equirect env map for GLB (`env_map.c`), a
  planar ocean mirror (`ocean.c`), tone mapping (`RenderLighting`).

## Architecture

New module `client/src/rt/` (headers in `client/include/rt.h`), GL resources owned there:

1. **`rt_scene.c` — acceleration structure (CPU).**
   - Bottom level per `Geometry` that needs triangles: a binned-SAH BVH over its local-space
     triangles, built once when the geometry is created, cached by `Geometry *`. Needs a CPU
     copy of positions: add `float *tri_positions` next to `tri_indices` in `Geometry`
     (`loaders.c`, `gltf.c` has `prim->base`, `prefab.c` cylinders/spheres, `terrain_mesh.c`,
     `ambient.c`, `map_post.c`). Freed with the geometry.
   - Top level over instances: one record per visible `Mesh` of the world scene: kind (box /
     plane / ramp / triangle BLAS id), world-to-local 3x4, local-to-world 3x4 (normals),
     world AABB, material index, flags (casts shadow, alpha-tested, double-sided, transparent,
     unlit/emissive). Caster/receiver flags come from the same prefab rules as
     `shadow_prefab_flags()` so RT shadows and shadow-map shadows agree. Rebuilt every frame
     from `scene->meshes` after `mesh_update_transform_matrix` (~2k instances: a linear BVH /
     Morton sort, well under 1 ms; the static part can be split out and refit only when a
     map object moves: `om_interp.c`, `map_post.c` moving objects, triggers).
   - Materials table: flat colour, emissive, opacity, alpha-test, texture atlas slot (phase 3).
2. **`rt_gpu.c` — upload.** BVH nodes, instances, triangles and materials go into `RGBA32F`
   textures read with `texelFetch` (core in ES 3.0 / WebGL2; no filtering needed). Static
   tables upload once per map, the instance/TLAS texture every frame with `glTexSubImage2D`.
   Sizes: 24k triangles + 2k instances fit easily in 2048x2048 RGBA32F (`GL_MAX_TEXTURE_SIZE`
   checked, graceful disable below 4096).
3. **`rt_prepass`** (in `rt_pass.c`): opaque world meshes drawn into a depth texture plus a
   world-normal `RGBA16F` colour attachment (MRT is ES 3.0 core) at the RT resolution
   (scene resolution times an RT scale setting, default 0.5). Reuses `shadow_update()`'s
   caster traversal pattern and the `shadow.vert` depth program with a normal output.
   Alternative considered and rejected for v1: tracing inside `basic.frag` per fragment (no
   prepass, but overdraw multiplies ray cost and nothing can be denoised).
4. **`rt_trace`** (`assets/shaders/rt_trace.frag`): a fullscreen pass that reconstructs the
   world position from depth, offsets along the normal, and traces per pixel:
   - sun shadow ray (jittered inside the sun's angular radius: soft shadows),
   - N cosine-weighted AO rays with a map-tunable radius,
   - optionally a reflection ray for pixels whose material is reflective (phase 3),
   - optionally shadow rays to the forward lights (phase 4).
   Output `RGBA16F`: R = sun visibility, G = AO, BA = spare (needs `EXT_color_buffer_float` on
   WebGL2, which every WebGL2 browser exposes; fall back to RGBA8 with 8-bit visibility).
   Traversal: stackless-friendly ordered traversal with a small fixed stack in registers,
   analytic box/quad/wedge tests for instances of the shared prefab geometries, triangle BLAS
   otherwise; alpha-tested materials sample their texture only in phase 3, before that they are
   treated as opaque (same as `shadow.c` today).
5. **`rt_denoise`** (`rt_denoise.frag`): temporal accumulation (previous view-projection,
   reprojection with depth and normal rejection, 8-16 frame history) then a 2-3 pass
   edge-aware à-trous blur. The history buffer is reset on map load, camera teleport, spectate
   switches and resolution changes. Without it, 1 sample per pixel per effect is far too noisy.
6. **Consumption in `basic.frag`.** A new sampler on a reserved unit (`RT_TEXTURE_UNIT`,
   documented next to `SHADOW_TEXTURE_UNIT 1`, read by `gl_FragCoord / rt_size`):
   - `rt_mode & RT_SHADOWS`: the shadow factor comes from R instead of `shadow_map` (the
     receiver uniforms of `shadow_apply_mesh_uniforms` stay; `no_shadow_receive` still applies;
     the first-person scene keeps the shadow map since it is drawn with its own camera).
   - `rt_mode & RT_AO`: G multiplies the ambient/indirect term; the `ambient_group` AO planes
     are hidden while RT AO is on so occlusion is not applied twice.
   - `rt_mode & RT_REFLECTIONS`: the traced radiance replaces `krunker_scene_env` /
     `use_env_map` for materials with `scene_env_intensity` / `env_map` / high metalness.
   Transparent meshes, sprites, unlit materials, UI quads and the viewmodel keep the current
   paths. Fog and tone mapping apply after, unchanged.
7. **Frame order** (`main.c` world pass, around line 2646): `shadow_update` -> `ocean_update`
   -> **`rt_frame()`** (TLAS rebuild + upload, prepass, trace, denoise; restores framebuffer,
   viewport, program, VAO, cull/depth/blend state like `shadow_update`) -> `sky_render` ->
   `scene_render(world)`. New `GPU_PASS_RT` in `gpu_timer.h` between PRE and SKY so
   `KRUNKNATIVE_GPU_TIMING=1` shows its cost. The scene target (`render_target.c`) is untouched;
   the RT pass has its own FBOs at RT resolution.

## Settings (Render tab, Box Office-only rows)

Same table style as `RS_B("vsync", ...)` in `ui_render_settings.c`, keys prefixed `rt`:

| key | field | values | default |
|---|---|---|---|
| `rtMode` | `rt_mode` | 0 off, 1 shadows, 2 shadows + AO, 3 + reflections | 0 |
| `rtScale` | `rt_scale` | 0.25 / 0.5 / 0.75 / 1 of the scene resolution | 0.5 |
| `rtSamples` | `rt_samples` | AO rays per pixel 1 / 2 / 4 / 8 | 2 |
| `rtDenoise` | `rt_denoise` | bool | 1 |
| `rtSunRadius` | `rt_sun_radius` | sun angular radius: 0 hard, 0.25, 0.5, 1, 2 degrees | 0.5 |
| `rtLocalLights` | `rt_local_lights` | bool: shadow rays to the forward point / spot lights (phase 5) | 0 |

Implemented (phase 1): the rows are CS_GAME entries of `client/src/ui/ui_client_settings.c` in a "Ray Tracing"
folder of the game's Render tab (basic and advanced views; before Post Processing where that folder exists),
stored in the game settings store and applied by `client_apply_render_setting` (`ui_render_settings.c`, with the
range clamps). Console (developer): `rt_mode [0-3]`, `rt_debug <bvh|normals|visibility|ao|history|off>`.
`KRUNKNATIVE_GPU_TIMING=1` prints the pass as `rt`.

Environment overrides for headless runs and tests: `KRUNKNATIVE_RT=<mode>`,
`KRUNKNATIVE_RT_SCALE` (both beat the store for the run, like `KRUNKNATIVE_VSYNC`), plus console commands `rt_debug bvh|normals|visibility|ao|history`
that blit the intermediate targets (`console.c` already has screenshot/draw commands).
`rt_mode` > 0 with an unsupported context (no float colour buffer, texture size too small)
logs once and runs as 0.

## Phases

Each phase builds, runs headless on Sandstorm and Kanji, and leaves `rt_mode 0` pixel-identical
(`make smoke`, `tools/uidiff` for the UI). Verify on the real GPU and under the ANGLE build
(`-DKRUNKNATIVE_GL_BACKEND=angle`) and `make web` from phase 2 on; llvmpipe hides driver bugs.

1. **Foundations (no visible change).** `tri_positions` retention, `rt_scene.c` BVH build with a
   CPU reference tracer and a unit test (`client_bin --rt-selftest`: random rays against the
   brute-force intersection of the same instance list, Sandstorm and Kanji), the float-texture
   upload, `rt_debug bvh` heatmap pass. Settings keys plumbed, mode 0 only.
2. **Ray-traced sun shadows** (mode 1): prepass, trace with the sun ray, temporal denoise,
   `basic.frag` consumption. Acceptance: no peter-panning or acne on Kanji's thin planes,
   contact-hardening soft shadows, players shadow the world and the world shadows players
   (today's static map only has map casters), frame cost <= 2.5 ms at 1080p / scale 0.5 on the
   2070 SUPER.
3. **Ambient occlusion** (mode 2): N cosine rays, à-trous spatial filter, AO planes hidden.
   Compare against `ambientShading` strips so the look stays Krunker-like (darker but not
   muddy): a map-level `rtAoRadius` default tuned on Sandstorm, Kanji, Burg.
4. **Reflections** (mode 3; landed 2026-11-02 as `client/src/rt/rt_atlas.c`, `assets/shaders/rt_reflect.frag`, `rt_refl_denoise.frag`: a fixed-cell 2048 px atlas drawn with the GPU filter, closest-hit rays that pass through transparent instances, sun + ambient shading at the hit, a roughness-scaled denoiser; `KRUNKNATIVE_RT_FORCE_REFL=<roughness>` forces every lit material for dev captures): material table with albedo and a texture atlas of the map's
   textures (one `glTexSubImage2D` packing at map load, 4096x4096 budget, missing = flat
   colour), one bounce shaded with sun + shadow ray + ambient, roughness-driven cone/jitter and
   the same denoiser; replaces `krunker_scene_env` for opted-in GLB materials and the skin
   `env_map`; the ocean keeps its planar mirror (cheaper and already exact).
5. **Local light shadows**: shadow rays to the `BasicLight` list (point/spot) for the forward
   lights, stored in the spare channels; rect-area lights stay unshadowed.
6. **Dynamic refinements**: static/dynamic TLAS split with refit, skinned glTF class models
   refit from `prim->base`, alpha-tested foliage in shadow rays (atlas lookup), map-object
   moves from the Object Manager and triggers marking the TLAS dirty.
7. **Optional hardware path (desktop only, later)**: a Vulkan ray-query accelerator behind
   `#ifndef KN_GLES` and a CMake option, sharing `rt_scene.c`'s instance list and the same
   output textures through GL/Vulkan memory interop. Only if phases 2-4 are visually right and
   the fragment tracer is the bottleneck at 1080p/1440p.

## Risks and open points

- **Parity**: this is a Box Office-only feature. It is isolated behind `rt_mode` and never
  changes the default frame; the UI row lives with the other Box Office-only rows.
- **Shader cost on web/Metal**: ES 3.0 fragment shaders have no dynamic indexing guarantees
  beyond uniforms and texelFetch; the traversal loop must use a fixed-size stack array and
  bounded loops (WebGL2 shader compilers unroll badly otherwise). Keep BVH depth <= 32.
- **Float textures**: sampling RGBA32F is core; *rendering to* RGBA16F needs
  `EXT_color_buffer_float` on WebGL2 (ANGLE and all current browsers have it). Fallback RGBA8.
- **Memory**: BVH + triangles for a 2k-object map is a few MB; the texture atlas for phase 4
  is the biggest item (64 MB at 4096x4096 RGBA8) and is phase-4-only.
- **MSAA**: the RT prepass is its own non-multisampled target, so `antiAlias` is unaffected;
  the RT result is sampled per pixel at scene resolution (bilinear from RT scale).
- **Denoiser correctness** is the main visual risk: ghosting on fast strafes needs motion
  vectors for players (phase 6); until then player pixels disable history (normal/depth test).
- **Build/CI**: no new dependencies; `make`, `make angle`-backed check and `make web` must all
  compile the new GLSL (keep `#version 450 core` on disk, ES-valid: no `f` suffixes).

## Files touched (expected)

- new: `client/include/rt.h`, `client/src/rt/{rt_scene,rt_gpu,rt_pass,rt_denoise}.c`,
  `assets/shaders/rt_prepass.{vert,frag}`, `rt_trace.frag`, `rt_denoise.frag`, `rt_debug.frag`
- `client/include/client.h` (`Geometry.tri_positions`), `client/src/loaders.c`, `gltf.c`,
  `prefab.c`, `terrain_mesh.c`, `ambient.c`, `map_post.c` (retain positions)
- `client/src/main.c` (world pass hook, fini), `client/include/gpu_timer.h` + `gpu_timer.c`
- `assets/shaders/basic.frag`, `client/src/materials/basic.c` (RT sampler + mode uniforms)
- `client/include/render_settings.h`, `client/src/ui/ui_render_settings.c`,
  `ui_settings.c` (keys), the Render tab rows in `client/src/ui/`
- `client/src/console.c` (`rt_debug`), `doc/raytracing-plan.md` -> `doc/raytracing.md` once built
