# Ray tracing (Krunker Box Office-only, opt-in)

As built, 2026-11-02. Plan and history: doc/raytracing-plan.md. Contract: `client/include/rt.h`.
Code: `client/src/rt/` (rt_scene.c, rt_gpu.c, rt_pass.c, rt_atlas.c, rt_self.c, rt_materials.c, rt_masks.c,
rt_selftest.c), the material table `assets/materials/` with its tools in `tools/rt-materials/`, and
`assets/shaders/rt_*.{vert,frag,glsl}`; consumers: `assets/shaders/basic.frag`,
`client/src/materials/basic.c`, `client/src/scene.c` (`rt_apply_mesh_uniforms`), `client/src/main.c`.

Portable GPU ray tracing in GLSL fragment shaders over a BVH stored in float textures
(`texelFetch`): no compute, no SSBO, no extension, so the same code runs on desktop GL 4.5,
ANGLE (Metal / OpenGL ES 3.0) and WebGL2. It is a Box Office-only feature (user decision
2026-11-02): the `rtMode` setting defaults to 0 and **the default frame must stay pixel-identical
to Krunker's** (every effect is gated by `rt_effects` bits that are 0 then; `rt_mode 0` runs none
of the passes). Effects in order of the modes: soft sun shadows (1), ambient occlusion (2),
reflections (3), plus shadow rays to the map's point / spot lights (`rtLocalLights`, any mode ≥ 1).

## Why a custom BVH is cheap here

Maps are 1000-2000 objects; 70-96 % are the cube prefab, which shares one unit geometry
(`g_cube_geometry`) scaled and rotated by the `Mesh` transform. Those are **analytic** instances
(ray vs unit box in local space, one 8-texel record, no triangles). Everything else (OBJ and glTF
models, cylinders, spheres, terrain, AO strips, builds, and also prefab planes and ramps, which
prefab.c builds as their own triangle geometry rather than `g_plane_geometry` / `g_ramp_geometry`)
is a triangle BVH. Typical sizes (self-test, release):

| map | instances (box / tri) | BLAS (nodes / tris) | TLAS nodes |
|---|---|---|---|
| Sandstorm | 1259 (892 / 367) | 10 (29.8k / 31.6k) | 2137 |
| Kanji | 1737 (1679 / 58) | 14 (19.0k / 19.9k) | 2999 |
| Burg | 1128 (858 / 270) | 12 (28.0k / 29.6k) | 2033 |

## Modules

| Module | Does |
|---|---|
| `rt_scene.c` | CPU acceleration structure: one binned-SAH BLAS per triangle `Geometry` (cached by pointer, leaves ≤ 4 triangles, depth ≤ 32, refit in place on `Geometry.tri_epoch`), one `RtInstance` per visible world mesh, the static / dynamic TLAS (below), the deduplicated material table (4096-bucket hash), a CPU reference tracer (`rt_scene_trace` / `rt_scene_occluded`) that walks exactly what the GPU gets, and the brute-force oracle |
| `rt_gpu.c` | packs the `RtScene` into five RGBA32F tables and uploads what changed (epochs below) |
| `rt_pass.c` | owns the `RtScene`, its GPU tables, the RT framebuffers and programs; runs the passes from the world pass; hands the results to basic.frag; the `rt_debug` views |
| `rt_atlas.c` | the texture atlas and the material rules of the reflection rays |
| `rt_self.c` | the local player's first-person shadow proxy: a third-person "puppet" of `me`, posed every frame like a remote player's model, traced as `RT_INST_SHADOW_ONLY` (below) |
| `rt_materials.c` | the material class table (`assets/materials/rt_materials.json`): lookup, per-`BasicMaterial` resolution cache, live reload (below) |
| `rt_masks.c` | per-weapon metal / roughness part masks bound as the weapon material's `metal_rough_texture` (below) |
| `rt_normals.c` | generated relief normal maps per base texture (Surface detail below) |
| `rt_filtering.c` | trilinear / anisotropic minification of cached textures while `rtMode` > 0 (Surface detail below) |
| `rt_selftest.c` | `client_bin --rt-selftest <map.json>` |
| `rt_common.glsl` | the traversal (TLAS → instance → analytic shape or BLAS), `#include`d by the trace shaders through shader.c's one-level resolver |
| `rt_prepass.vert/.frag` | depth + world normal (+ reflection material, + motion vector) of the opaque world at RT resolution |
| `rt_trace.frag` | sun shadow, AO and local-light rays |
| `rt_reflect.frag` | the reflection ray and its hit shading |
| `rt_gi.frag` | the one-bounce diffuse GI ray and its hit shading (half RT resolution) |
| `rt_taa.frag` | the TAA resolve (run by `render_target.c`, not by `rt_pass.c`) |
| `rt_denoise.frag`, `rt_refl_denoise.frag`, `rt_gi_denoise.frag` | temporal + à-trous denoisers |
| `rt_bump.frag` | builds a relief normal map from a texture's luminance |
| `rt_shafts.frag`, `rt_shafts_denoise.frag`, `rt_shafts_composite.frag` | the sun-shaft march, its temporal pass and the composite |
| `rt_bloom.frag` | the bloom mip chain (run by `render_target.c`) |
| `rt_mask_classify.frag`, `rt_mask_filter.frag` | the run-time weapon part masks |
| `rt_atlas.frag`, `rt_debug.frag` | atlas cell copy; the debug blit |

Geometry keeps a CPU copy of its triangles for the builder: `Geometry.tri_positions` /
`tri_vertex_count` / `tri_indices` / `tri_epoch`, filled by `geometry_set_triangles()` (loaders.c)
at every world-geometry creation site (OBJ, glTF, prefab, terrain, zone, ambient strips, map_post
builds, class models, ...). Only world-scene geometry is retained; UI / HUD / editor / sky geometry
is not.

## GPU tables (`rt_gpu.c`, `rt.h` "GPU table layout")

Every table is one texture of width `RT_TEX_WIDTH` (2048), grown by rows, nearest, no mips, read with
`texelFetch`. The node tables (`tlas`, `blas`) are RGBA32UI (`RtTable.integer`); the others are RGBA32F with
integers stored as floats (exact below 2^24). Element i of a table with T texels per element starts at texel i·T.

| table | texels / element | layout |
|---|---|---|
| `tlas` | 2 (`RT_NODE_TEXELS`, RGBA32UI) | a node holds its two CHILDREN's slots, one texel each (left, right): xyz = the child's box as half floats (`packHalf2x16` pairs: (min.x, min.y), (min.z, max.x), (max.y, max.z); min rounded down and max up to the next half, so a box never shrinks), w = `RT_SLOT_INNER` &#124; node index for an inner child, else `first << 11` &#124; `count` for a leaf child (count 0 = an empty slot). Node 0 is the root; TLAS leaf primitives are global instance indices. A CPU leaf node is entered only as a root (a tiny TLAS, the self-shadow subtree, a one-leaf BLAS) and packs itself as its left slot |
| `blas` | 2 (RGBA32UI) | every BLAS's nodes packed one after the other in the same child-slot form; rt_gpu adds the BLAS's node base to the inner links and `tri_first` to the leaves' first triangle (rt_scene keeps both local) |
| `tris` | 3 (`RT_TRI_TEXELS`) | (v0, 0), (v1, 0), (v2, 0) in the geometry's local space, BLAS after BLAS |
| `inst` | 8 (`RT_INST_TEXELS`) | t0..t2 world_to_local rows (3x4), t3..t5 local_to_world rows, t6 = (kind, packed BLAS root or −1, material, flags), t7 spare |
| `mats` | 4 (`RT_MAT_TEXELS`) | t0 = (colour, opacity), t1 = (emissive, alpha_test), t2 = (atlas code, uv_size.xyz) (see Atlas; `rt.h`'s t2 comment still describes the planned u0 v0 u1 v1 form), t3 = (metalness, roughness, reflect_intensity, unlit) |

Instance kinds: `RT_INST_BOX` (unit cube: x, z in [−0.5, 0.5], y in [0, 1]), `RT_INST_PLANE`
(the y = 1 face, +y front), `RT_INST_RAMP` (the wedge y + z ≤ 0.5 in that box), `RT_INST_TRIANGLES`.
Flags: `CASTS_SHADOW` (shadow.c's prefab caster rules through `shadow_mesh_casts()`, non-map meshes
such as players always unless transparent), `DOUBLE_SIDED`, `ALPHA_TEST`, `TRANSPARENT` (never
occludes; reflection rays pass through), `DYNAMIC` (moved / not a map object: the denoiser keeps
less history), `UNLIT` (reflections return its colour as-is).

Uploads, by the `RtScene` epochs: `static_epoch` → blas, tris, mats in full (a BLAS or material
appeared); `refit_epoch` → only the refit BLAS's node and triangle rows; `static_tlas_epoch` → tlas
and inst in full (the static half was rebuilt); otherwise per frame only the root node, the dynamic
half's nodes and the dynamic instance rows. Each table keeps its packed rows on the CPU so a partial
`glTexSubImage2D` of whole rows carries the right neighbours.

### Traversal (`rt_common.glsl` `rt_tlas_trace`)

- **Child slots:** a node is entered without testing its own box (its parent did), so a step is 2 fetches and 2
  slab tests (`rt_node_visit`); the old layout fetched a popped node's own box again plus both children's (6 fetches,
  3 tests).
- **Near child first:** the nearer hit child becomes the current node and only the farther one is pushed.
- **One stack:** a triangle instance pushes the rest of its TLAS leaf and a sentinel, then the loop continues in
  `rt_blas` with the instance's local ray (an affine map keeps the ray parameter, so `t_max` carries over) until the
  sentinel pops back to the TLAS. `RT_STACK` 32 entries for both levels instead of 24 per level: the stacks live in
  local memory, and that traffic, not the texel fetches, dominated the traversal (a per-entry `t_enter` stack for
  pop culling made it twice as slow and was dropped).
- **One instance fetch:** t6 (kind, BLAS root, material, flags) is read once per instance; occlusion queries on solid
  casters skip the world normal and position (`rt_record`).
- The CPU tracer (rt_scene.c) keeps the plain two-texel node form; the layout is the GPU's only, so `--rt-selftest`
  still checks the BVH build and the CPU semantics. GPU correctness is checked by pinned-camera captures (identical
  within the denoiser's run-to-run noise; Kanji pixel-identical).

### Static / dynamic TLAS and refit

Instances `[0, static_instance_count)` are map objects (`shadow_mesh_casts(mesh) >= 0`) that have
not moved for `RT_STATIC_SETTLE_FRAMES` (60) frames; their SAH BVH is built once and sits at TLAS
nodes `[1, 1 + static_tlas_count)`. Everything else (players, pets, builds, a spinning / trigger /
Object-Manager-moved object while it moves) follows and gets a small per-frame BVH after the static
nodes; node 0 is a root inner node over both halves, an empty half a count-0 leaf with an inverted
box. A per-mesh `MeshState` (transform, previous transform, moved / seen frame, static slot)
drives the split; map objects start settled, a member that moves, appears, disappears or is refit
dirties the static half (rebuilt lazily, at most once per frame, ~1 ms on Kanji). The traversal is
the same for both halves.

A posed glTF (skinned / morphed class models, pets) refits its BLAS in place: gltf.c's dynamic
upload copies the posed positions into `tri_positions` and bumps `tri_epoch`; rt_scene re-reads
the triangles in leaf order (`RtBlas.tri_order`), recomputes the boxes bottom-up and bumps
`refit_serial` / `refit_epoch`. A changed triangle count builds a new BLAS and leaves the old entry
dead until the map unloads.

Per-frame CPU cost (release, RTX 2070 SUPER): rebuild + upload 0.02-0.04 ms on the three maps
(0.75 ms before the split); the whole `rt` CPU column ~0.3 ms, mostly the prepass draw submission.

## Passes and frame order

From main.c's world pass, after the sky (`GPU_PASS_SKY`), just before `scene_render(client->scene)`
and after the sync-hide / spectate / third-person swaps so the traced set equals the drawn set:
`rt_frame()` (marked `GPU_PASS_RT`), then `rt_begin_world()` … `scene_render` … `rt_end_world()`.
The first-person scene is never part of the BVH; it gets its own pass (Viewmodel tracing, below). `rt_frame` with `rt_mode 0`
(unless `rtTaa` is on: see the velocity-only path), no sun, or an unsupported context returns at once.

1. **Build + upload**: `rt_scene_build` over `client->scene`'s visible meshes plus `rt_self_frame()`'s
   shadow-only puppet meshes (first person only: `rt_set_self_caster` from main.c, off when dead,
   spectating or when the own model is already in the world scene), `rt_gpu_upload`,
   `rt_atlas_update` (pending cells).
2. **Prepass** (`rt_prepass`): every traced instance's mesh is drawn into targets of size
   scene × `rtScale`: depth (`GL_DEPTH_COMPONENT24`), attachment 0 = world face normal
   (RGBA16F, the same dFdx/dFdy normal basic.frag lights with; `.w` = dynamic flag), attachment 1
   (mode 3) = reflection material (reflect intensity, roughness, metalness), attachment 2 = motion
   vector (RGBA16F: `.xy` uv delta to last frame from the instance's previous matrix × the previous
   view-projection, `.z` last frame's view depth, `.w` written). `glDrawBuffers` is rebuilt from
   whatever is attached. Transparent and sprite meshes are skipped.
3. **Trace** (`rt_trace.frag`, fullscreen triangle): per pixel the world position from depth +
   inverse view-projection, offset along a silhouette-safe normal (`surface_normal()`: rebuilt from
   the nearer depth neighbours where the prepass normal straddles an edge), then
   - R = sun visibility: one any-hit ray towards `-g_render_lighting.light_direction`, jittered
     inside the sun's angular radius (`rtSunRadius`); 0 where N·L ≤ 0;
   - G = 1 − AO: `rtSamples` cosine-weighted hemisphere rays of length `RT_AO_RADIUS` (12 units,
     tuned against Krunker's ambient strips), occluders weighted (1 − t / radius), occluders nearer
     than `RT_AO_MIN` (0.5) ignored (decals / planks proud of a face); skipped at mode 1;
   - B = traversal steps / 256 (the `bvh` debug view), A = 1 on a traced surface;
   - attachment 1 (local lights, RGBA32F): the packed visibilities (below).
   Occluders are instances with `CASTS_SHADOW` and without `TRANSPARENT`.
4. **Reflect** (`rt_reflect.frag`, mode 3 only, pixels with reflect intensity > 0; the pass
   early-outs otherwise): one GGX-jittered closest-hit ray, shaded at the hit as basic.frag's
   Lambert path would see it (albedo = colour × atlas texel, ambient + sun N·L with one shadow
   ray, unlit / emissive as-is, fog by camera distance + ray length, the sky colour fading to fog at
   the horizon on a miss), into an RGBA16F target. Transparent instances are skipped (glass,
   liquids and the AO strips lying on every face would otherwise black out every reflection).
4b. **GI** (`rt_gi.frag`, mode ≥ 2, `RT_EFFECT_GI`; targets at RT resolution / `RT_GI_DIVISOR` 2):
   one cosine-weighted closest-hit ray per lit pixel; hit = albedo × atlas × (ambient + sun N·L
   with one shadow ray) / π + emissive × `RT_GI_EMISSIVE`, fogged by ray length (the hit's full
   ambient stands in for the bounces not traced); miss = mix(ambient / π, sky, `RT_GI_SKY_MIX`).
   **Energy:** the radiance L estimates the bounce irradiance G = π·L, but Krunker's maps are lit
   for a strong constant ambient A, and one physical bounce off ~0.4-albedo walls returns far less
   than A indoors (replacing 65 % of A with the bounce cost a Sandstorm interior a third of its
   brightness). So basic.frag's `krunker_gi_irradiance` keeps A's luminance as a floor, uses G's
   colour as a tint and lets G raise the level only where it is brighter than A (a sunlit floor's
   bounce, an emissive sign):
   E = A·t · lum(A) / lum(A·t) · max(1, lum(G) / lum(A)), t = mix(1, (G / lum(G)) / (A / lum(A)),
   `RT_GI_TINT` 0.6) (Rec. 709 luminance, t clamped to [0, 4]); maps without ambient get E = G.
   AO multiplies E once. Measured mean luminance vs mode 1 (Sandstorm open / Sandstorm interior /
   Kanji): before 0.91 / 0.68 / 0.88, after 0.99 / 0.99 / 1.00 at mode 2. Output RGBA16F (rgb, a =
   hit distance), denoised by `rt_gi_denoise.frag` (temporal with the motion-vector reprojection,
   then three widening 5x5 à-trous steps with depth / normal stops). GI, AO, local-light and world
   reflection rays never see the self-shadow puppet (below).
5. **Denoise** (`rt_denoise.frag`; `rtDenoise` off = the raw trace): pass 0 temporal: reproject
   with the motion vector (camera-only without it), blend where the stored view depth agrees
   (disocclusion), history length ≤ 32 on static pixels, ≤ 16 on dynamic pixels under velocity
   reprojection, ≤ 4 camera-only; a 3x3 neighbourhood clamp on R (a shadow sweeping over static
   ground would otherwise trail; penumbrae span [0, 1] and are not clamped).
   **Temporal gradient (2026-11-03):** a moving caster changes the occlusion / bounce / reflection of a
   static surface that reprojects perfectly, so a long history kept the old lighting there for up to
   half a second. AO (G), GI and reflections now clip their history to this frame's 3x3 band (mean
   +- 2 sigma for AO, 2.5 sigma for GI, 2 sigma for reflections, plus a small floor against the
   few-sample noise; untraced -1 AO entries skipped) and a clipped history restarts at 4 frames (3 for
   reflections). A converged static history sits inside the band and keeps its full length: frame-to-
   frame flicker with a still camera is unchanged (Sandstorm, user settings: 14-18 vs 10-13 px of 2 M
   at 2 % fuzz). Reflections also reproject with the motion vector now (world pass only; the viewmodel
   pass has no velocity), so a moving reflective mesh keeps its own history. Pixels of a moving mesh
   (prepass normal.w) trace the full AO sample count each frame instead of half (their history is too
   short for the two-frame AO split). `KRUNKNATIVE_RT_NO_GRADIENT=1` restores the previous policy for
   A/B. Cost: +0.04 ms at the default settings, +0.25 ms at rtScale 1 / resolution 1 (1080p, mode 3).
   Then à-trous 5x5 edge-aware steps with depth / normal stops: step 1 and 2 on R and G, a third
   (step 4, G only, wider depth reach, softer normal stop) when AO is on. Pass 2 is the temporal
   step of the local-light target (no spatial filter). The reflection target has its own temporal
   + roughness-scaled à-trous (`rt_refl_denoise.frag`: a mirror stays sharp). History is reset on
   map load, resize, mode change.
6. **Hand-off**: `rt_begin_world()` binds the results; `rt_apply_mesh_uniforms(mesh)` (scene.c,
   per draw, next to the shadow uniforms) sets basic.frag's `rt_effects` (0 for transparent
   meshes, which sit in front of the traced surface and keep the shadow map), `rt_inv_viewport`
   (the lookup is `gl_FragCoord.xy / scene viewport`, bilinear from the smaller RT target) and
   the samplers; `rt_end_world()` draws the debug view if one is set and clears the state.
7. **Velocity-only path** (`rtTaa` on with `rtMode` 0): `rt_frame` runs the build and the prepass
   alone (no BVH upload, no trace, `rt_effects` stays 0) so `render_target.c` can read the motion
   vectors through `rt_taa_velocity()`. On entry `rt_frame` snapshots texture units 1..6
   (`save_units`) and restores them: it runs after `shadow_begin_world()`, and before that fix its
   state restore unbound the sun shadow map from unit 1 (visible as dark sunlit faces on this path
   and on transparent meshes at any mode).

Texture units: `RT_TEXTURE_UNIT` 14 (`rt_result`: R visibility, G AO), `RT_LOCAL_TEXTURE_UNIT` 15
(`rt_local`), `RT_REFL_TEXTURE_UNIT` 16 (`rt_refl`; needs `GL_MAX_TEXTURE_IMAGE_UNITS` ≥ 17, else
reflections stay off with a log line), `RT_GI_TEXTURE_UNIT` 17 (`rt_gi`; needs ≥ 18, else GI stays
off with a log line). Inside `rt_frame` the passes use units 0..7 only; units 1..6 are restored to
what the world pass had bound.

What basic.frag does with the bits (`rt_effects` 0 = the old path, byte for byte):
`RT_EFFECT_SHADOWS` (1): `krunker_shadow_mask()` returns `rt_result.r` instead of the shadow-map
compare (receivers and `no_shadow_receive` as before). `RT_EFFECT_AO` (2): `krunker_rt_ao()`
scales the ambient / light-map irradiance of `krunker_lambert` and the scene-env irradiance only
(direct sun, lights, emissive untouched); basic.c hides Krunker's ambient_0/1 AO planes meanwhile so
occlusion is not applied twice (`rt_world_effects()`). `RT_EFFECT_REFLECTIONS` (4):
`krunker_scene_env`'s radiance and `use_env_map`'s texel come from `rt_refl` (the DFG / F0
weighting and `env_map_intensity` stay). `RT_EFFECT_LOCAL_LIGHTS` (8): each point / spot light's
irradiance in the light loop is multiplied by `krunker_rt_local(slot)`. `RT_EFFECT_FORCE_REFL`
(16, dev only): 35 % of the traced radiance blended into every lit surface. `RT_EFFECT_GI` (32):
`krunker_lambert`'s indirect irradiance becomes `krunker_gi_irradiance(krunker_ambient_irradiance(n)) * ao`
(the ambient's luminance as a floor, tinted and raised by the bounce: formula under pass 4b). `RT_EFFECT_SKY_AMBIENT` (64): `krunker_ambient_irradiance(n)`
returns hemispherical irradiance from the map's sky instead of the constant AmbientLight: with a sky
dome (`sky_ambient_mode` 1) the PMREM table's roughest level (env_map.c) sampled along the normal
(top colour up, fog at the horizon, fog × 0.25 below as the ground tint); without one (mode 2) the
same shape from `sky_color` / fog. basic.c's `basic_sky_ambient_scale` matches the luminance of an
upward normal to Krunker's `ambient_color * ambientI` (Rec. 709; `env_map_sky_up`), so overall
exposure is unchanged (mean luminance within 1.5 % on Kanji / Sandstorm / Burg) and only hue and
direction dependence change; light-mapped materials and `ambient_enabled` off keep their path.

`RT_EFFECT_MATERIALS` (128): a material with a Box Office class (below) runs basic.frag's GGX
branch with the class factors whatever the lighting setting, plus the class's indirect specular
(`krunker_rt_mat_specular`).

Effect bits summary (all in `rt.h`): 1 shadows, 2 AO, 4 reflections, 8 local lights, 16 force-refl
(dev), 32 GI, 64 sky ambient, 128 material classes (with the generated relief normal maps), 1024
bevelled cube edges, 8192 world sky visibility at mode 1 (opt-in), 16384 the viewmodel's sky probe (also the water /
glass draws' sky reflection), 32768 water and glass (set per draw on transparent water / glass meshes only). What each `rtMode` sets (rt_pass.c `rt_frame`):

| rtMode | turns on |
|---|---|
| 0 | nothing (Krunker's frame); `rtTaa` alone runs the velocity-only prepass |
| 1 | sun shadows, sky ambient, the first-person self shadow, material classes with relief normal maps (when the table loaded), bevelled cube edges, texture filtering, the filmic highlight shoulder, specular occlusion of the viewmodel's sky reflection, water / glass fresnel + refraction with the sky reflection |
| 2 | adds AO, GI, and specular occlusion of the world's sky reflection from the traced AO |
| 3 | adds reflections (the water / glass surfaces' own traced reflection included) |

`rtLocalLights`, `rtBloom`, `rtShafts` and `rtSharpen` need mode ≥ 1; `rtTaa` works at any mode.

## Local light shadows (`rtLocalLights`)

Up to `RT_LOCAL_MAX` (8) point / spot lights of basic.frag's forward list (`basic_lights_list()`,
the same filter and slot order as the uniform upload), the nearest to the camera, go to the trace
pass as (position, range; distance 0 → `RT_LOCAL_RANGE_CAP` 2000) and (spot axis, cone cosine;
w = −2 for a point light), with (decay, penumbra cosine, luminance as basic.c scales it, incl.
`light_unit_factor` and correct_lights). Per light one any-hit ray from the surface to a jittered
point on a `RT_LOCAL_LIGHT_RADIUS` (0.5 unit) disk at the light; skipped (visibility 1) when out of
range, behind the surface, outside the cone, when the attenuated irradiance × N·L / π is below
`RT_LOCAL_MIN` (1/255: it would not show), or when the ray would be shorter than 0.05 units.
With the denoiser on, at most `RT_LOCAL_PER_FRAME` (4) slots are traced per frame (a window
rotating with the frame index, `rt_local_mask`); the temporal step holds the others' history.
Packing: one RGBA32F target, two lights per channel at 12 bits each,
`channel = floor(v0·4095 + 0.5)·4096 + floor(v1·4095 + 0.5)` (exact in a float), nearest; basic.frag
fetches the four nearest texels once per fragment and unpacks per light (`rt_local_index[16]` maps
a light slot to its traced index, −1 = untraced = 1). If the RGBA32F attachment is incomplete the
effect stays off (logged once). Rect-area and directional map lights are not shadowed; the spot
penumbra is not in the shadow (only the cone cut-off).

### Muzzle-flash and explosion lights (`vfx.c`, `rt.h` `RT_FLASH_*`)

While ray tracing is on (`rtMode` ≥ 1), every gunshot puts a brief warm point light (`RT_FLASH_COLOR`
#ffcc88, peak `RT_FLASH_INTENSITY` 1.8, cutoff `RT_FLASH_RANGE` 45 units, decay 2, life
`RT_FLASH_LIFE_MS` 60 ms, linear fade from the first rendered frame) `RT_FLASH_FORWARD` (1 unit) ahead
of the muzzle, and every explosion (`vfx_explosion`) an orange one (#ff9944, peak 3, range ≥ 45 units
and 1.5 × the blast radius, 160 ms). Gunshot lights follow the `muzzleFlash` setting; Krunker has
neither, so with `rtMode` 0 none is ever added (the frame stays Krunker's).
- **Where:** your own shot uses the first-person barrel tip (`arms_point` on the held weapon's muzzle,
  the same point the tracer leaves from), else your eye 1.5 units along the aim (third person, class
  cameras, a hidden weapon); a remote shot uses the shooter's eye 1.5 units along the shot toward its hit.
- **Forward lights:** each is a `BasicLight` with `transient = 1` in basic.c's forward list (up to
  `RT_FLASH_MAX` 4 live; the oldest gives way; none when the map's lights fill all 16 slots). basic.frag's
  light loop lights the world and the first-person weapon and arms with it (the viewmodel never gets
  local-light shadows, so your gun is lit unshadowed by its own flash).
- **Physical-units maps** (`correctLights`, 1/d² falloff): the intensity is scaled so the light matches
  the legacy (1 − d/range)² falloff at `RT_FLASH_PHYS_REF` (6 units), so a flash looks alike on both.
- **Shadows (`rtLocalLights`):** `select_local_lights` takes the transient lights apart, nearest first,
  and gives at most `RT_FLASH_TRACE_MAX` (2) of them the traced slots after the map's lights. The map's
  lights keep their slots and histories while a flash lives, the round robin windows over the map's
  lights only, and the flash slots are shot every frame and skip the temporal history
  (`rt_local_fresh` in `rt_denoise.frag`: the current visibility, so a flash never smears into the
  next flash elsewhere). Without `rtLocalLights` the flash still lights, unshadowed.
- **Knob:** `KRUNKNATIVE_RT_NO_FLASH_LIGHT=1` (dev) adds none; `KRUNKNATIVE_VFX_LOG=1` logs each light's
  spawn point and per-frame intensity.

## Reflections and the atlas (`rt_atlas.c`)

Which materials reflect (`rt_scene.c material_index` + `rt_atlas_material`): a v7 store skin's
`env_map` → reflect_intensity = `env_map_intensity`, metalness 1, roughness 0.15 (a metal mirror);
else a v10 GLB env material → reflect_intensity = `scene_env_intensity` with the glTF
metalness / roughness factors; else 0. Everything else pays nothing (the reflect pass early-outs,
and mode 3 costs the same as mode 2 on Sandstorm / Kanji, which have no reflective material).

A reflection ray that hits a textured object needs its texture, and a fragment shader cannot bind
the map's ~100 textures, so every world texture a traced material uses is copied once into one
`RT_ATLAS_SIZE` (2048²) RGBA8 atlas of `RT_ATLAS_SLOTS` (256) fixed `RT_ATLAS_CELL` (128 px) cells,
drawn with the GPU's mip filter (ES 3.0 cannot query texture sizes, so cells are drawn, not read
back); each cell's mean colour is read back once (`glReadPixels` RGBA/UNSIGNED_BYTE) into a
256 x 1 texture. Material t2 = (slot + 1 + 4096 · uv_mode, uv_size.xyz), 0 = no texture; uv_mode
1 = Krunker cube UVs, 2 = plane UVs, rebuilt from the hit's local position (no texture repeat /
offset / rotation / flips), 0 = the mesh's own UVs, which the triangle table does not carry: such
hits (OBJ / glTF) reflect the cell's mean colour. Slots are assigned lazily as the map's objects
build, drawn at the next `rt_frame`, and reassigned per map. Sandstorm uses ~50 slots.

## Material classes (`rt_materials.c`, `assets/materials/rt_materials.json`)

Krunker's content has no material maps: a gun is one OBJ with one flat texture, a map cube is a
colour or a texture. With ray tracing on, a hand-authored table gives materials physical
properties. Each class is metalness, roughness, `reflect` (0..4, the tracer's reflect intensity;
0 = no reflection ray) and an optional `f0`, a metal's minimum specular colour (default 0.45 for
metalness ≥ 0.5) so black-painted weapon textures still read as metal:
F0 = mix(0.04, max(albedo, f0), metalness).

Schema:

```json
{ "classes":  { "<class>": { "metalness": 0.9, "roughness": 0.35, "reflect": 1.0, "f0": 0.45, "note": "..." } },
  "textures": { "<texture base name or glob, e.g. brick_0*>": "<class>" },
  "models":   { "<assets-relative model path glob, e.g. models/weapons/*>": "<class>" },
  "weapons":  { "<weapon id>": "<class>" },
  "parts":    { "player_body": "...", "player_head": "...", "player_arms": "...", "player_legs": "...", "cosmetic": "..." },
  "paint":    { "<paintShader 0..5>": "<class>" },
  "default":  "<class>" }
```

Precedence: paint > weapons > models > textures > parts > default. Globs are `*` only; within a
section the first match in file order wins, so exceptions go before their family glob. Unknown
class names and `_comment` keys are skipped. Texture keys are bare base names (`wall_0_3`), taken
from loaders.c's texture cache; the other parts of the key come from hints set where meshes are
built (`rt_materials_hint_weapon` / `_model` / `_part` in player_mesh.c, gltf.c, prefab.c). The
resolution is cached on the `BasicMaterial` (`rt_class`, `rt_metalness`, `rt_roughness`,
`rt_reflect`) and redone when its texture, paint shader or the table changes. Unlit and sprite
materials get no class; a glTF material with its own metallicRoughness map keeps it unless the map
is a Box Office part mask (`rt_mask`).

The shipped table has 15 classes (metal, dark_metal, chrome, gold, paint_fx, wood, cloth, skin,
plastic, stone, glass, window, water, emissive, foliage), about 180 texture rules, 14 model rules,
every v10 weapon id plus the Box Office ones, the player parts and the six paint shaders (Gold →
gold, Silver → chrome, the shader paints → paint_fx); the default is stone. Rough classes (wood,
stone, cloth, skin, foliage, emissive) have `reflect` 0: their GGX highlight and the sky specular
are enough, and a traced ray on every matte pixel would make mode 3 cost like the forced-mirror
worst case. The tracer's material table takes metalness / roughness / reflect from the class when
no Krunker env rule (skin `env_map`, GLB `scene_env_intensity`) applies.

Shading (basic.frag, `RT_EFFECT_MATERIALS`): the class factors drive the GGX branch (`pbr_active`),
diffuse = albedo × (1 − metalness); the indirect specular (`krunker_rt_mat_specular`, three's DFG
weighting) comes from the traced reflection when the class reflects and the reflect pass ran,
else from the sky: the PMREM table along the reflection vector at the material's roughness, or the
sky_color / fog blend without a dome, or the flat ambient without the sky ambient, scaled like
`krunker_ambient_irradiance` so a rough metal keeps the energy a Lambert surface would have had
instead of going black. A class's `f0` raises the metal's specular colour to max(albedo, f0), so a
black-painted gun reflects that sky as gunmetal grey. Where the traced reflection has no data
(alpha 0: the reflect pass skipped the pixel) the sky estimate fills in, blended by the alpha at the
traced region's edges. The class's `reflect` only decides whether a ray is traced; it does not
dim the incoming light, since the DFG weight already sets how much the surface reflects. Values
above 1 (chrome, gold, glass, water) act as an exposure boost on both the traced and the sky
radiance, so rtMode 1 and 3 agree in level. Indoors they still differ in content: rtMode 1
reflects the open sky even under a roof, rtMode 3 the actual surroundings, which are darker.

Editing: change the JSON and run the developer console command `rt_materials_reload` (classes
re-resolve on the next draw). `KRUNKNATIVE_RT_MATERIALS_LOG=1` prints every resolution
(material → class and why), `KRUNKNATIVE_RT_NO_MATERIALS=1` leaves the bit off for A/B captures.

### Authoring tool (`tools/rt-materials/`)

`contact_sheet.py` renders contact sheets of every map / prop texture, weapon texture and melee
texture with a proposed class (name keywords first, then colour statistics) and writes a draft
JSON grouped by texture family; the shipped table was corrected by eye from those sheets. See
`tools/rt-materials/README.md` for regeneration and conventions.

### Per-weapon part masks (`rt_masks.c`)

One class per weapon would make a whole gun metal. A mask splits it into parts: a
metallicRoughness map (R unused, G roughness, B metalness) per weapon / melee MODEL, bound as the
weapon material's `metal_rough_texture` (unit 5) while `rtMode` > 0, so the class factors are
multiplied per texel (barrel and receiver metal, grip and stock wood or rubber). The mask is keyed
by the model (`weapons/weapon_2`, `melee/melee_0`, a remodel `weapons/weapon_1_3`), not the
texture, so a store skin that swaps the texture keeps the base model's mask.

The classifier sorts each texel of the base texture in HSV into metal (G 90, B 200: metalness
tops out at 0.78 so unreflected metal stays dark-worn rather than black), painted metal, wood (hue
8–50°, saturation ≥ 0.30), polymer (true black) and plastic (saturated non-warm hues), then a 5x5
majority filter, then per-texture overrides from `assets/materials/weapon_masks.json` (rules, UV
rectangles, defaults; e.g. the Combat Knife's wrapped grip → polymer). Sources, in order: a
hand-painted `textures/<model key>_mr.png` override if present; else the mask is built at run time
from the base texture's pixels with the same rules as `tools/rt-materials/weapon_masks.py` (the
Python tool is the reference and the sheet viewer; keep both in step), lazily on the first mesh
that needs it, cached per model key. The run-time build is two fullscreen GPU passes (`rt_mask_classify.frag`, `rt_mask_filter.frag`) over the loaded texture, identical to the Python tool's output; if they fail to compile a C classifier does the same on the CPU (`KRUNKNATIVE_RT_MASK_CPU=1` forces it). Bound at player_mesh.c (weapon and melee
meshes) and prefab.c (map weapon pickups, loadMesh assets). A mesh built while ray tracing was off
keeps no mask until it is rebuilt (respawn, class / skin change, map load).
`KRUNKNATIVE_RT_NO_MASKS=1` uses the class factors alone; `KRUNKNATIVE_RT_MASK_DUMP=<dir>` writes
every built mask to diff against the Python tool's.

## Viewmodel tracing (first-person weapon and arms)

The weapon and arms (`client->fps_scene`, drawn with the fps camera after the world) are not in the
BVH, so the world pass cannot light them. `rt_viewmodel_frame(fps_scene, &fps_camera)` (main.c,
inside the viewmodel block after `shadow_begin_world()`) runs, with `rtMode` ≥ 2 and a full trace
that frame (the material-level bits, sky ambient and material classes, reach the fps draw at any
`rtMode` > 0 even without these passes): a prepass of the fps scene's meshes with the fps camera (its own fov; vertex normals
where the VAO has them), then the GI (mode ≥ 2) and reflect (mode 3) passes from those surfaces into
the world's tables. It uses a second target set swapped in for the duration (`vm_swap`), because
the world's velocity attachment still feeds the TAA resolve after the fps draw. The weapon is
camera-locked, so its reprojection is the identity and its history builds to the static cap. GI
runs at a quarter of the RT resolution and with a 400-unit ray reach (`RT_VM_GI_FAR`), and the
fullscreen passes are scissored to the weapon's projected rectangle. `rt_begin_viewmodel()` /
`rt_end_viewmodel()` bracket the fps draw: it gets `RT_EFFECT_REFLECTIONS` / `RT_EFFECT_GI` as
traced plus the world's material-level bits (sky ambient, material classes), never shadows, AO or
local lights (the viewmodel keeps the shadow map). The viewmodel's rays skip the self-shadow
puppet: its arms and rifle stand where the first-person weapon is (in the third-person pose), so
rays leaving the gun started inside them and the gun reflected that dark geometry. The gun reflects
the world only, which matches Krunker showing no body in first person. `KRUNKNATIVE_RT_NO_VIEWMODEL=1` skips it for A/B captures.

## Emissive lights (GI next-event estimation)

Neon signs and other emitters light their surroundings directly, as part of GI (`RT_EFFECT_GI`,
rtMode 2 and up). Before this, the one cosine ray per pixel found a sign only by chance, so Kanji's
signs glowed (bloom) but barely tinted the walls next to them.

- **The list** (`rt_scene_emissive_lights` in `rt_scene.c`, every frame): instances flagged
  `RT_INST_UNLIT` (Krunker's MeshBasicMaterial signs and light bars, or any non-zero emissive),
  not transparent, not the self-shadow puppet, with radiance luminance at least
  `RT_EMISSIVE_MIN_LUM` (0.08). The radiance is what a GI bounce that hits the instance returns:
  material colour x the texture's atlas mean (`rt_atlas_mean_rgb`) + emissive, times
  `RT_EMISSIVE_LIGHT_SCALE`. Each is scored by luminance x surface area / (distance to the camera^2
  + its size^2), and the best `RT_EMISSIVE_MAX` (64) are kept as oriented boxes: the instance's local
  bounds (the unit cube, the plane's y = 1 face, or the BLAS root box) through `local_to_world`.
  Kanji has 773 emissive objects, Sandstorm lists 8, Burg 1.
- **The table** (`rt_gpu_upload_emissive`): one RGBA32F row, 5 texels a light: centre + power
  (luminance x box area), the three half-axes with the summed squared size and the instance index,
  and the radiance. Bound on unit 9 (`RT_EMISSIVE_UNIT`) during the GI pass only.
- **Sampling** (`emissive_direct` in `rt_gi.frag`): one pass of weighted reservoir sampling picks a
  light with probability proportional to power x a soft facing term / (distance^2 + size^2). A box
  face that faces the point is picked in proportion to its area, then a uniform point on it, and one
  shadow any-hit ray toward it. The estimate is `Le cos_s cos_l / d^2 x A_face / (p_face p_light)`,
  with the geometric term capped at a full hemisphere (2 PI): one sample right next to a small light
  otherwise turns into blotches. A fog factor attenuates it with distance.
- **No double counting:** a bounce ray that hits a listed light drops that light's emission, since
  the sampled estimate covers it. There is no MIS: the cosine bounce still carries the emitters
  outside the list at their plain strength.
- **Additive, not a tint:** basic.frag's `krunker_gi_irradiance` keeps the ambient's luminance as a
  floor and only raises it where the GI is brighter, so a dim bounce plus a sign's light that still
  sums below the ambient would only have tinted. Where the sampled light is at least 5 % of the
  ambient, `rt_gi.frag` first lifts the bounce to the ambient's luminance (the floor basic.frag applies
  anyway), so the light lands on top: E is about A + E_emissive. It uses the map's constant ambient
  for that, not the sky-dome ambient; pixels away from every emitter are unchanged.
- **Scale:** `RT_EMISSIVE_LIGHT_SCALE` is 4. Signs are drawn with display colours that clip at 1,
  while real neon is far brighter than the walls around it; at 1 the light under a Kanji
  billboard was invisible against Kanji's ambient, at 4 the floor and pillar beside it are clearly lit
  and not blown out.
- Knobs: `KRUNKNATIVE_RT_NO_EMISSIVE_LIGHTS=1` leaves the list empty for A/B captures;
  `KRUNKNATIVE_RT_EMISSIVE_LOG=1` logs the first frame's list (instance, centre, size, radiance).
- Limits: one light sample per pixel (at half the RT resolution), so the light from several small
  emitters next to a surface is noisy until the GI denoiser has accumulated it; emitters are treated
  as uniformly bright boxes, so the light ignores the shape of the letters on a sign.

## Surface detail and sampling

These four run whenever `rtMode` > 0 and have no setting of their own (each has a dev knob for A/B
captures). None of them changes the RT prepass or the BVH: rays still leave from and hit the real,
hard-edged geometry.

### Generated relief normal maps (`rt_normals.c`, `rt_bump.frag`)

Krunker's textures carry no normal maps, so walls and floors are flat under the sun and the GGX
highlights. While `RT_EFFECT_MATERIALS` is on, every classed material whose base texture has a class
`bump` > 0 gets a tangent-space normal map built from that texture on the GPU:

- height = the texture's luminance, sampled bilinearly between texel centres, with a half-texel tent
  blur (weight 0.5; `KRUNKNATIVE_RT_BUMP_BLUR=<0..1>` overrides it) so single-pixel noise does not
  read as relief;
- slope from a 3x3 Sobel one target texel apart, at `RT_BUMP_SLOPE` (3.0) per luminance step: bright
  reads raised, so mortar and seams sink;
- built at 1-4x the texture size until the longer side reaches `RT_BUMP_FINE_MAX` (512), so
  pixel-art steps become soft one-texel bevels instead of aliased edges; mipmapped, repeat wrap, so
  far walls flatten instead of shimmering. The size comes from `stbi_info` on the texture cache's path
  (ES 3.0 cannot query a texture's size); textures the cache does not know, and the blank texture, get
  no map.

Built lazily: a material asks during its draw, the texture is queued, and `rt_normals_frame()` builds
it at the next `rt_frame` with the GL state saved and restored, so a new texture's first frame has
no relief. Each map costs 0.5-2 ms once (6.6-25.6 ms per map load in all, including the first shader
compile); the world pass pays about +0.04 ms at 1080p.

Binding (`materials/basic.c`): only for a classed material with no normal map of its own and
`maps_flip_v` 0, on the existing normal unit (`BASIC_NORMAL_TEXTURE_UNIT` 6) with `normal_scale` = the
class's `bump`. basic.frag's `krunker_perturb_normal` applies it with screen-space derivatives, so it
needs no tangents and follows whatever UV the base texture is sampled with (cube uv modes, repeats,
flips, rotation). The `bump` key of `rt_materials.json`, per class:

| class | bump |
|---|---|
| stone | 1.0 |
| wood | 0.7 |
| foliage | 0.5 |
| dark_metal | 0.45 |
| metal | 0.4 |
| cloth | 0.3 |
| plastic, gold | 0.25 |
| window, paint_fx | 0.2 |
| chrome, skin | 0.15 |
| glass, water, emissive | 0 |

`KRUNKNATIVE_RT_NO_BUMP=1` turns it off. Maps are dropped on map unload.

### Bevelled cube edges (`RT_EFFECT_BEVEL` 1024)

70-96 % of map objects are scaled unit cubes with razor-sharp edges. `krunker_rt_bevel()` in basic.frag
tilts the shading normal of a cube face toward its neighbour face within `RT_BEVEL_WIDTH` (0.6) world
units of an edge, linearly up to the 45-degree bisector at the edge itself, so edges catch a thin
highlight from the sun and GGX and darken on the shadow side. It runs right after the normal-map
perturbation and adds to that normal, so relief detail survives.

- The face is the local axis the geometric normal is closest to; world sizes come from the
  `transform` matrix. On small faces the width shrinks to `RT_BEVEL_FACE_FRACTION` (a quarter) of
  the face's shorter side, so a 1-unit plank keeps a flat face; near-zero scales are skipped.
- `prefab_build_cube` stores the cube's face mask in `BasicMaterial.rt_bevel_faces`; an edge against a
  face the map masked out (the box touches another object there) is not bevelled. Only map cubes set
  it; every other mesh stays 0.
- `rt_apply_mesh_uniforms` uploads 0 for alpha-tested, unlit, sprite, flat-normal and wireframe
  materials. Transparent meshes and the first-person scene never get the bit.

Cost about +0.13 ms on the world pass at 1080p (measured under load). At the default 0.6 scene
resolution the bevel is 1-2 pixels wide. `KRUNKNATIVE_RT_NO_BEVEL=1` turns it off.

### Texture filtering (`rt_filtering.c`)

Krunker minifies and magnifies its map textures with `NearestFilter` and no anisotropy
(`deob-10.0.6.js:159855-159856`, also :159005-159006 and :159793-159794; three applies
`texture.anisotropy` only when set, :81806), so far floors and fences shimmer and break into moiré.
While `rtMode` > 0, `rt_filtering_update()` (every `rt_frame`) walks the texture cache, records each
texture's own min filter, generates mipmaps where a texture has none (the prefab composites), and sets
`GL_LINEAR_MIPMAP_LINEAR` plus anisotropy up to `RT_FILTER_ANISOTROPY` (8x) where
`EXT_texture_filter_anisotropic` exists (desktop, ANGLE, WebGL's own name). The mag filter stays
nearest, so close-up textures keep Krunker's pixel look. Textures loaded later are picked up when the
cache grows. With `rtMode` 0 each recorded min filter and anisotropy 1 come back, pixel-identical to
Krunker's even after toggling in one run.

Texture parameters were chosen over a sampler object on unit 0: a sampler would also override wrap and
min filter of clamped textures, render targets and video frames that have no mipmaps, which would then
sample black. `texture_placeholders_check` (loaders.c) regenerates mipmaps after it re-uploads a
missing asset's checker, so that texture stays complete. Not covered: textures outside the cache
(video frames, worn-skin effect stand-ins, glyphs). `KRUNKNATIVE_RT_NO_FILTER=1` keeps Krunker's
sampling with `rtMode` > 0.

### Specular occlusion (`RT_EFFECT_SKY_VIS` 8192, `RT_EFFECT_SKY_PROBE` 16384)

A classed material's indirect specular (`krunker_rt_mat_specular`) falls back to the sky PMREM where
no traced reflection covers the pixel, which made metal under a roof reflect open sky.
`krunker_rt_spec_occlusion()` scales that sky term, and the occluded part is replaced by the radiance
of nearby shaded surfaces (the pixel's GI bounce when GI runs, else half the flat ambient), so it goes
warm-dark like the traced reflection rather than black. The occlusion comes from:

- **rtMode ≥ 2, world:** the traced AO through Lagarde's specular occlusion,
  `so = saturate(pow(NdotV + ao, exp2(-16 roughness - 1)) - 1 + ao)`.
- **The first-person viewmodel, any mode (`RT_EFFECT_SKY_PROBE`):** the weapon is small and
  camera-locked, so one "ambient cube" of six sky visibilities is traced from the camera on the CPU
  (+-x, +-y, +-z; `RT_SKY_PROBE_RAYS` 8 rays each in a cosine lobe, 48 a frame, `RT_SKY_VIS_RADIUS`
  60 units), smoothed over frames by `RT_SKY_PROBE_BLEND` (0.15), and evaluated per pixel along the
  reflection direction. The probe rays are closest-hit queries that step past transparent instances
  (Krunker's ambient strips lie on every floor) and never start in the self-shadow puppet, which
  contains the camera. A single upper-hemisphere value was tried first and failed: a weapon mostly
  mirrors near-horizontal walls, not the sky above. CPU cost about +0.04 ms, no GPU cost.
- **rtMode 1, world (`RT_EFFECT_SKY_VIS`), opt-in only with `KRUNKNATIVE_RT_SKY_VIS=1`:** one ray per
  pixel in a cosine lobe about the mirror/normal halfway, written to `rt_result.g` (unused at mode
  1) on one 8x8 tile of each 16x16 block a frame, with `rt_denoise.frag` holding the rest. It costs
  about +1.7 ms at 1080p / 0.5 (+0.9 ms at an 8-unit reach), far over budget, and the tile pattern
  shows in single frames, so it is off by default. Without it the world's classed metal at mode 1
  keeps the unoccluded sky (mode 2+ is occluded by AO, mode 3 traces reflections).

On the first-person AK under Sandstorm's overhang, mode 1 went from bluish grey (B−R +7.9) to neutral
gunmetal (−0.7); it stays lighter than mode 3's traced walls because the occluded part uses half the
flat ambient, not the real walls. `KRUNKNATIVE_RT_NO_SPEC_OCCLUSION=1` turns all of it off.

### Cinematic surface detail (`rt_cinematic()`)

With the Cinematic preset (doc's "Cinematic preset" section; `rt_cinematic()` in rt.h) a classed material's draw sets
`rt_cine` (basic.c), and surfaces read as their material on purpose. Custom presets are untouched: every change
below is gated on `rt_cine`. Dev knob `KRUNKNATIVE_RT_NO_DETAIL=1` keeps the preset without them for A/B.

- **Stronger relief:** the generated normal map's scale doubles, capped at 1.6 so pixel art stays relief, not noise.
- **Cavity:** `rt_bump.frag` writes a cavity term into the generated map's alpha (the height against its 3x3
  neighbourhood); `krunker_rt_cavity` darkens the low parts up to 45 %, so mortar, plank gaps and panel seams read.
- **Procedural micro-detail on map cubes** (`krunker_rt_detail`, by the class's new `detail` key in
  rt_materials.json: `stone` for stone, `metal` for metal / dark_metal, `wood` for wood): stone gets broad mottling,
  pitting and grain; metal brushed streaks along the face's long side (also varying its roughness); wood warped grain
  lines. It works in world units on the cube's face, so neighbouring blocks continue one pattern. Full strength on
  untextured flat-colour cubes (most of Kanji, Sandstorm's sand), a third on textured ones. Each octave fades out
  before it would alias, using the pixel footprint (`fwidth`).
- **Per-object colour variation:** +/-4 % brightness hashed from the cube's position (untextured; a third textured),
  so a field of equal blocks stops reading as one fill.
- **Bevels:** width 1.0 unit, capped at 0.3 of the face (was 0.6 and a quarter), plus contact creases
  (`krunker_rt_crease`): within 0.6 units of an edge where the map masked the neighbour face (the box touches another
  object), the face darkens up to 22 %.
- **Gloss:** metal and smooth surfaces (metalness >= 0.5 or roughness < 0.6) get 0.1 less roughness; rough classes
  (stone, wood, cloth) stay matte. The sun's GGX highlight on metal and glass follows.
- **Cost:** the GPU world pass at a 1920x1080 window on Kanji, minimum of three runs each way, 0.15-0.17 ms with the
  detail vs 0.15-0.21 ms without: within noise (machine load 5-15).

## Water and glass (`RT_EFFECT_WATER` 32768)

Krunker draws liquids and glass as plain alpha blends over whatever is behind them. With `rtMode` > 0 the
transparent water and glass surfaces of the world scene get a fresnel blend of a reflection and a
refracted view of the scene behind them (rt.h "water and glass", rt_pass.c `water_frame` /
`rt_water_copy_scene` / `water_effects`, basic.frag `krunker_rt_water`).

- **Which surfaces.** A transparent `BasicMaterial` whose class is `water` (kind 1) or `glass` / `window`
  (kind 2), and every transparent Krunker LIQUID plane: prefab.c gives each liquid a wave height, and most
  map liquids are untextured colours that no texture rule can class (the Oasis pool resolved to the default
  `stone`), so the table's `water` class shades them. A glowing liquid (an emissive colour) keeps Krunker's
  look. Opaque liquids are in the prepass already and keep the opaque world's class shading and traced
  reflection.
- **Refraction.** scene.c calls `rt_water_copy_scene` once, between the opaque and the transparent draws of
  the world scene; when a water / glass mesh is visible it blits the opaque frame into a texture of the same
  format (RGBA8, or RGBA16F under `rtBloom`; an MSAA frame is resolved by the blit). basic.frag samples it at
  the fragment, offset by the wave normal's tilt from flat in view space (`RT_WATER_REFRACT`; glass panes
  are flat and not offset), and rejects an offset landing on something in front of the surface by the world
  prepass depth. Water thickness (prepass depth behind minus the fragment's depth) turns the view through it
  toward the water colour: `body = max(opacity, 1 - exp(-thickness / RT_WATER_DEPTH))`; glass keeps its
  authored opacity.
- **Reflection.** Schlick fresnel, F0 0.02 (water) / 0.04 (glass). At `rtMode` 3 the surfaces get their own
  traced reflection: a prepass of up to `RT_WATER_MAX` (64) of those meshes on a third target set (`g_wt`,
  swapped in like the viewmodel's) at 1 / `RT_WATER_DIVISOR` (3) of the RT resolution, then the shared
  reflect and reflection denoise passes with the camera's real reprojection (water is world-fixed). Meshes
  past the cap, and every mode below 3, use the sky estimate of the material sky-specular path, occluded by
  the camera's directional sky probe (the per-pixel AO and sky visibility describe the opaque surface
  behind, not the water).
- **Composite.** Coverage `w = F + (1 - F) x body`; the surface colour (reflection and lit body) goes through
  basic.frag's usual fog and tone map, then `out = surface x w + refracted x (1 - w)` with alpha 1, since the
  refracted texel is not the one under the fragment. Without this frame's copy the shader falls back to
  alpha `w` and the hardware blend. Since the water / glass draws now get `RT_EFFECT_SKY_AMBIENT` and the
  class shading, they are lit like the rest of the traced frame; before, transparent meshes kept Krunker's
  flat ambient while every opaque surface around them was traced.
- **Units** during those draws: the copy on `RT_WATER_SCENE_UNIT` (rt_result's, unread by water), the world
  prepass depth on `RT_WATER_DEPTH_UNIT` (rt_local's), the water reflection on `RT_REFL_TEXTURE_UNIT`.
  Transparent draws follow every opaque draw of the world scene, and `rt_frame` unbinds those units at the
  start of the next frame.
- **Test content.** 311_oasis's pool (a transparent untextured liquid, opacity 0.6) and 311_industry's 66
  transparent `window_0` windows. `KRUNKNATIVE_RT_NO_WATER=1` keeps Krunker's alpha blend for A/B.

## Physical sky (`rtSky`)

`rtSky` (Box Office-only switch, off by default, needs `rtMode` > 0 and a supported tracer; env override
`KRUNKNATIVE_RT_SKY=1`) replaces Krunker's gradient dome or clear colour with a physically based sky drawn by
`rt_sky.frag` (shared math in `rt_sky.glsl`, CPU side in `client/src/sky_physical.c`), called first from
`sky_render`. A map with a textured `skyDomeTex` dome keeps Krunker's picture.

- **Atmosphere:** Rayleigh single scattering with horizon whitening, a limb-darkened sun disk along
  `g_render_lighting.light_direction` (so it matches the shadows) of radius max(`rtSunRadius`, 0.4 degrees), a
  sharp Henyey-Greenstein aureole (g 0.95), a drifting fbm cloud layer that advances with `textureAnim`, and
  faint stars on night maps.
- **Map mood:** brightness always follows the map's authored gradient. A day factor from the authored mean
  luminance and the sun's elevation decides the hue: night maps (Kanji: 0.12) keep their authored hue and only
  gain the glow and stars; day maps (Sandstorm, Burg: 1.0) take the atmosphere's hue, except that the authored hue's
  share (`psky_keep`, C and GLSL in step) rises from 40 % (15 % under Cinematic) to 85 % where the authored colour
  is saturated (HSV saturation 0.08..0.25). So Sandstorm keeps its peach horizon under its authored blue zenith
  (the physical sky had turned it into a blue noon), while Burg's near-grey clear colour still becomes a blue sky.
  `sky_physical_warmth()` (the authored horizon's red over blue) turns off the Cinematic grade's cool shadow tint
  on warm maps.
- **Fog:** `krunker_fog` in basic.frag brightens and warms toward the sun and dims slightly away from it (Mie
  forward scattering, normalised to a mean of about 1). It is gated by the `psky_on` uniform rather than an
  effect bit, so transparent meshes get it too.
- **Lighting follows the sky:** `env_map.c` modulates getEnvMap's PMREM rows per elevation by the physical
  sky's brightness ratio and half its hue shift (`env_map_refresh` rebuilds the table when the sky changes),
  so sky ambient, the specular fallback and the sky probe see it; reflection misses show the full sky; GI
  misses keep the old brightness and take the new hue.
- **Cost:** the sky pass goes from 0.011 to 0.057 ms at 1080p. Mean frame brightness at mode 2 moves -1.0 to
  -3.0 % (Sandstorm, Kanji, Burg).
- **Limit:** the sun glow of the volumetric shafts (`rt_shafts.frag`) uses its own phase function; it points
  the same way but is not tied to the sky's aureole.

## Volumetric sun shafts (`rtShafts`)

Box Office-only switch, off by default, any `rtMode` ≥ 1 (`rt_pass.c` "volumetric sun shafts" block,
`assets/shaders/rt_shafts*.frag`, constants `RT_SHAFTS_*` in `rt.h`). Three steps:

1. **March** (`rt_shafts.frag`, inside `rt_frame` after the GI pass, counted in the `rt` timer column), at
   RT resolution / `RT_SHAFTS_DIVISOR` (4). Each pixel walks its view ray from the camera to the prepass
   surface, or to the fog's far distance for the sky, with `RT_SHAFTS_STEPS` (10) samples. Each sample
   shoots one any-hit sun shadow ray with the same traversal, sun jitter and alpha-tested cut-outs as the
   sun shadow. The self-shadow puppet's subtree is included, so your own shadow cuts the shafts. Two
   media share the march:
   - **Scattering haze.** Krunker's fog is THREE.Fog, `smoothstep(fog_near, fog_far, d)`, which is almost
     nothing within the first 100 units, so on its own it would only put shafts in the far distance. The
     in-scattered light is carried by a thin exponential haze `M(d) = 1 - exp(-density d)` with
     `density = RT_SHAFTS_DENSITY (4) / fog_far`, so a map's fog distance sets how hazy it is. Samples are
     spread evenly in `M` (its inverse, jittered per pixel and frame), so each weighs `M(d) / N`.
   - **Krunker's fog.** Each sample's slice also records its share of Krunker's fog, `F(b+1) - F(b)`.

   Output RGBA16F: R the lit haze, G the haze share, B the lit share of Krunker's fog, A the march
   distance.
2. **Temporal** (`rt_shafts_denoise.frag`): the march end point is reprojected into the previous camera.
   History is rejected when its stored march distance differs by more than 10 %, which stops a sky
   history from sliding onto roof edges as a halo. A 3x3 neighbourhood clamp, then 1/10 of the new frame.
3. **Composite** (`rt_shafts_composite`, called by `main.c` after the world draw, before the world's
   effects and the first-person scene, into the scene target): a joint-bilateral upsample guided by the
   RT prepass depth. Where no tap is on the pixel's surface it adds nothing. Two blended draws:
   - `+ sun_light * RT_SHAFTS_STRENGTH (0.35) * (RT_SHAFTS_ISO + HG(cos, RT_SHAFTS_G 0.6) / HG(1)) * R`.
     The phase is normalised to 1 toward the sun.
   - `- fog_color * RT_SHAFTS_SHADOW (0.4) * (F(d) - B)`, as `GL_FUNC_REVERSE_SUBTRACT`. This removes part
     of the map's own fog where the sun does not reach it, so shadowed volumes darken when looking away
     from the sun too.

   Because the shafts land in the scene target before `render_target_end`, `rtBloom` blooms them. The
   viewmodel is drawn afterwards, so it is not hazed beyond its own fog. Alpha is masked off.

A map without fog (no `fogD`) gets a faint additive haze instead (`RT_SHAFTS_HAZE` 0.3 of the strength,
far distance `RT_SHAFTS_HAZE_FAR` 1500) and no darkening, since there is no fog to remove. Every shipped
map sets `fogD`. `KRUNKNATIVE_RT_SHAFTS=1` turns the switch on for a run.

## Tone mapping and bloom

**Filmic shoulder** (any `rtMode` > 0, `basic.frag` `krunker_filmic_shoulder`, uniform `rt_filmic` set by
`basic.c` for every draw of the traced world and the viewmodel, transparent ones included). Every shipped map
leaves Krunker's tone mapping at None (Linear only scales by the exposure), so its colours are display values and
anything above 1 clips per channel, which turns orange sunlit sand yellow and burns GGX highlights to flat white.
A full ACES curve was rejected: on display-referred content it re-grades every map (a 0.9 white wall becomes
0.74 at exposure 1). The shoulder leaves everything below 0.8 untouched and rolls the rest off exponentially to
1 with slope 1 at the knee, on the brightest channel with the colour scaled by it (the hue holds), then
desaturates what lies far above 1 toward white (half-way at 3x over). Maps that pick Reinhard, Cineon or ACES
keep their own curve; the exposure still applies. Measured mean frame brightness at mode 1, shoulder off → on:
Kanji 59.83 → 59.77, Sandstorm 75.80 → 75.47, Burg 134.64 → 134.34 (0–255, pinned cameras), so it rides on
`rtMode` with no switch of its own; `KRUNKNATIVE_RT_NO_FILMIC=1` keeps Krunker's curve for A/B. There is no eye
adaptation: brightness stays constant for competitive play.

**Bloom** (`rtBloom`, off by default, needs `rtMode` ≥ 1; `render_target.c`, `assets/shaders/rt_bloom.frag`).
While it runs, the scene target, its MSAA buffer and the TAA history are RGBA16F and
`render_target_bloom_active()` is 1; `basic.c` then sets `rt_bloom_out` for opaque draws of the traced world and
viewmodel, and `basic.frag` writes rgb clamped to 1 and alpha = 1 + weight, weight = how far the colour went
above 1 before the tone curve + `smoothstep(0.25, 1, max(emissive))` (Kanji's neon signs carry an emissive
colour; dim emissive tints stay out), faded by the fog. Opaque draws do not blend, so alpha is free for it;
transparent draws keep their opacity in alpha, and a transparent layer blended over a glowing pixel lowers the
weight in proportion to its coverage (smoke dims a sign's glow, and over the sky, alpha 1, it never creates one).
Additive draws may leave rgb above 1 in the float target, which also counts. `render_target_end` runs the chain
after the TAA resolve (the resolve carries the weight in alpha, `rt_taa.frag keep_alpha`, so the glow does not
shimmer with the jitter): a 13-tap prefilter into half resolution extracting rgb·weight + max(rgb − 1, 0) with
Karis-averaged 2x2 groups (one sub-pixel highlight cannot flash the bloom), 13-tap downsamples to 1/64, 3x3
tent upsamples added level onto level, then `scene_composite.frag` adds level 0 × `BLOOM_STRENGTH` (0.5) to the
clamped scene colour. A context without half-float colour targets (or a refused half-float MSAA buffer) logs
once and runs without bloom. The Nettspend laced composite ignores bloom.

## Upscaling and sharpening (`rtSharpen`)

Krunker renders the scene at `resolution` (0.6 by default) and the composite stretches it to the window
with bilinear sampling, which is the main reason the frame looks soft. `rtSharpen` (Off / Low / Medium /
High, off by default, needs `rtMode` ≥ 1; `render_target.c`) replaces that stretch with two passes,
both written from the published description of AMD FidelityFX Super Resolution 1 (no FSR source is used):

1. `assets/shaders/rt_upscale.frag`, EASU-style edge-adaptive upscaling into a window-size RGBA8 target:
   12 taps around the input position (the 2x2 bilinear quad and the 8 texels ringing it), an edge
   direction and length from luma at the four quad texels (each against its cross neighbours, blended
   bilinearly), a windowed lanczos-2 approximation evaluated in a frame rotated onto the edge and
   stretched along it (so edges are interpolated along their length and stay sharp across it), then a
   clamp to the 2x2 quad's min / max so it cannot ring. Inputs are clamped to 1 like the composite.
2. `assets/shaders/rt_sharpen.frag`, RCAS-style contrast-adaptive sharpening onto the window: a 5-tap
   cross whose negative lobe is the largest that keeps the result inside the neighbourhood's range
   (capped at 0.1875), scaled by `sharpness = 2^-stops` (Low 1, Medium 0.5, High 0.2 stops). Bloom is
   added after the sharpening so the glow stays soft, then the `saturationn` / `hue` filter, as in
   `scene_composite.frag`.

When the scene is not scaled (resolution 1, no aspect override) the upscale is skipped, and the
sharpening runs only with `rtTaa` on (TAA softens the frame). The Nettspend laced composite keeps its own
path; with `rtSharpen` Off or `rtMode` 0 the composite is unchanged. `KRUNKNATIVE_RT_SHARPEN=<0-3>` sets the
level for a run.

Quality, 1920x1080 window, resolution 0.6, `rtMode` 1, PSNR against a resolution 1.0 render of the same
pinned view (higher is closer; an unsharpened 1.0 render is the reference, so strong sharpening scores lower
even when it looks crisper):

| view | bilinear | Low | Medium | High |
|---|---|---|---|---|
| Kanji, whole frame | 34.73 | 35.45 | 35.23 | 34.86 |
| Sandstorm, right half (away from the animated billboard) | 44.61 | 44.91 | 44.03 | 42.82 |
| Sandstorm, whole frame | 30.35 | 30.31 | 30.25 | 30.12 |

Low is the closest to the full-resolution render; Medium and High go crisper than it. No halos or ringing
in 2x crops of the HOTEL sign and the crates; High emphasises the stair-steps a 0.6 render without
anti-aliasing has (TAA or MSAA removes them first).

## Cinematic preset (`rtPreset`, `rt_cinematic()`)

Every effect above is tuned to stay close to Krunker's brightness (GI keeps Krunker's flat ambient as a floor, the
sky ambient and the physical sky are luminance-matched), so each one alone changes little. `rtPreset` (Ray Tracing
folder: Custom / Cinematic, default Custom; env `KRUNKNATIVE_RT_PRESET=cinematic`) is the look-first alternative:
it stops matching Krunker's brightness. `rt_cinematic()` (rt.h) is 1 when it is selected, the stored `rtMode` is
above 0 and the tracer is supported; `rtMode` 0 still runs none of it.

**Implied settings.** `client_render_settings()` returns the settings in effect: under the preset a copy of the
stored values with rtMode 3, `rtScale` `RT_CINE_RT_SCALE` (0.4), `rtSamples` 2, `rtLocalLights`, `rtTaa`,
`rtBloom`, `rtSky`, `rtShafts` on, `rtSharpen` Low and scene resolution `RT_CINE_RESOLUTION` (0.6). The store is
never written, so Custom returns to the player's own rows; `client_render_settings_stored()` reads the store. Dev
tuning knobs: `KRUNKNATIVE_RT_CINE_RES`, `KRUNKNATIVE_RT_CINE_SCALE`, `KRUNKNATIVE_RT_CINE_NO=shafts,bloom,local,taa,sky,sharpen`.

**Lighting** (`RT_EFFECT_CINEMATIC` 65536 on the world, viewmodel and transparent draws; basic.frag `krunker_lambert`,
rt_pass.c `cine_uniforms`; constants `RT_CINE_*` in rt.h):
- indirect = fill + GI bounce, times AO: the ambient is kept at `RT_CINE_FILL` (0.7) of Krunker's, in the map's
  authored ambient hue with the directional sky ambient's luminance (the physical sky's blue alone turned warm maps
  cold), and the GI bounce is added at its real brightness (no ambient floor, no lift in rt_gi.frag);
- AO is softened to ao^0.6 and reaches `RT_CINE_AO_RADIUS` (14): full AO with the lower fill crushed small rooms;
- the sun is `RT_CINE_SUN` (1.1) brighter and warmed toward the horizon (amber below ~35 degrees);
- GI hits use the same fill, misses see `RT_CINE_GI_SKY_MIX` (0.1) of the sky, emissive light is `RT_CINE_EMISSIVE` (2x);
- exposure: a fixed per-map factor from the map's lighting (`cine_exposure`: Krunker's estimate amb + sun / 2 over
  the preset's, times `RT_CINE_EXPOSURE_BIAS` 1.0, 0.8 on night maps by `sky_physical_day`, clamped 0.6..1.8), no
  eye adaptation; applied before the colour pipeline's curve (`rt_cinematic_exposure()` for the sky passes).

**Measured** (1920x1080 window, headless, release, RTX 2070 SUPER, pinned camera -77,140,-347):

| map | lower-frame p10 / p50 / p90 (0-255) | p90 / p10 |
|---|---|---|
| Sandstorm, mode 0 | 22 / 48 / 71 | 3.2 |
| Sandstorm, Cinematic | 26 / 55 / 110 | 4.2 |
| Kanji, Cinematic | 33 / 63 / 98 | 3.0 |
| Burg, Cinematic | 63 / 120 / 209 | 3.3 |
| first-person interior (Sandstorm), mode 0 / Cinematic | 23 / 45 / 58 vs 16 / 34 / 73 | 2.5 vs 4.6 |

GPU per frame with a quiet GPU: Custom mode 3 7.1-7.5 ms; Cinematic 7.3 ms at resolution 0.6 / rtScale 0.4 (9.3 at
rtScale 0.5, 12.7 at resolution 0.75, 21 at resolution 1.0, 61 at resolution 1.0 with rtScale 1).

**Polish pass (2026-11-03).**
- *GI bug:* rt_frame assigned `g_rt.effects` only after the passes, so the world GI pass saw the previous frame's
  0 and ran Custom GI under Cinematic (fill 1.0, sky share `RT_GI_SKY_MIX` 0.5, emissive 1x). Sandstorm's shaded
  roof read blue (33,38,44 vs Krunker's 27,25,24) because half of each GI miss was the physical sky's blue zenith.
  The bits are now assigned before the prepass; the shaded roof is 27,26,27.
- *Kanji's dirty floors:* K3's stone detail had a 7-unit octave at 0.55 with a 0.32 albedo contrast, which read as
  room-sized stains on flat-colour floors. It is now 0.12 (the 1.7-unit octave carries the look, the 0.4-unit
  grain is 0.22) and the stone contrast 0.22. GI and the emissive lights were not the cause (A/B with
  `KRUNKNATIVE_RT_NO_DETAIL` / `_NO_GI` / `_NO_EMISSIVE_LIGHTS`).
- *Sandstorm's mood:* see "Physical sky": a saturated authored sky keeps its hue.
- *Kanji's floor streaks:* the stone detail's normal gain (0.45) bent slab-scale noise enough that grazing moonlight
  drew diagonal streaks across the flat floors; stone now uses 0.1 and reads through its albedo grain. The roof's
  variation is the HOTEL sign's bloom glow, not dirt (it vanishes with `KRUNKNATIVE_RT_CINE_NO=bloom`).
- *Glowing first-person gun:* the direct GGX sun highlight was the whole source (white pixels drop to 0 without
  it; the indirect specular barely matters), because Cinematic made metal 0.1 glossier and the panels turned into
  sun mirrors. Metal now keeps its class roughness under Cinematic (plastic / painted still get the step), and
  non-emissive over-brightness blooms at `RT_CINE_SPEC_BLOOM` (0.35), so mostly emissives and the sun bloom. K4's
  repro (Kanji, cfg kn-k2/cfg-all, 300x300 receiver crop): Custom 3248 white pixels, Cinematic 6544 before, 0 after.
- Measured after the pass, 1920x1080, Sandstorm: Krunker 0.65-0.69 ms, Custom mode 3 7.2-7.5 ms, Cinematic
  7.4-8.4 ms GPU (load 3-5).

## Cinematic colour pipeline (`rt_grade.c`, `rt_grade.glsl`)

The colour half of the Cinematic preset (`rt_cinematic()`; the lighting half, `rt_cine_*` in basic.frag and
`cine_exposure` in rt_pass.c, is the preset's own section). Everything here is gated on `rt_cinematic()`: the
Custom preset and `rtMode` 0 never see it.

- **Tone curve.** For maps whose own curve is None / Linear (every shipped map), basic.frag's `krunker_cine_curve`
  replaces the filmic shoulder: three.js's ACES fit at `RT_CINE_EXPOSURE` (1.05) after the preset's per-map
  exposure, blended 60 % toward a hue-preserving version (the input's channel ratios scaled to the curve's peak).
  Plain per-channel ACES drifted sunlit sand and yellow neon toward white. The sky passes (`sky.frag`,
  `rt_sky.frag`) run the same curve at `rt_grade_exposure()` (RT_CINE_EXPOSURE x `rt_cinematic_exposure()` x a
  Linear map's exposure), so the sky and the world agree.
- **Grade.** A 32^3 RGBA8 3D LUT built on the CPU when the map's mood changes (the physical sky's day factor,
  `sky_physical_day()`, quantised to 5 %; maps without a physical sky use the same formula on the clear colour),
  applied with a 0.15 vignette in the final composite (`scene_composite.frag` / `rt_sharpen.frag` include
  `rt_grade.glsl`), after bloom and before the player's own saturation / hue filter, on unit 2. Day: a mild S-curve,
  warm highlights and a hint of cool shade (luminance kept), +10 % saturation (+22 % in the mid tones) and a
  2.5 % black lift so players in shade are not crushed. Night: the authored hue, a toe (`pow 1.12`) for deeper
  blacks, saturated colours (neon) lifted by up to 30 %. No grain, aberration, motion blur or depth of field.
- **Bloom.** Strength `RT_CINE_BLOOM_STRENGTH` (0.65), upsample radius 1.4, and a display-luminance knee
  (`RT_CINE_BLOOM_KNEE` 0.9, `rt_bloom.frag` `knee`) on top of the HDR weight, so the sun disk and the brightest
  highlights glow softly. A knee of 0.72 made the whole sky bloom into a white haze.
- **Sky.** `psky_cine` (rt_sky.glsl, mirrored in `sky_physical_base` for the PMREM table; the cache key carries
  the flag): the day colour 15 % back to the authored hue instead of 40 %, +20 % saturation, fuller clouds
  (coverage smoothstep 0.45..0.74, opacity 0.85) and a stronger aureole (2.5x reach, 0.6 amplitude). Night maps
  keep their authored mood (the day factor gates it).
- **Shafts.** Density x1.5 and strength x1.3 (`RT_CINE_SHAFTS_*`; 2x / 1.8x hazed the whole frame), coloured by
  the physical sky's reddened sun (`sky_physical_sun_color`) with a sharper lobe (g 0.75) so their glow agrees
  with the aureole.
- **Cost.** The composite with the grade: post pass 0.375 -> 0.42 ms at 1080p (RTX 2070 SUPER, load ~7-12).
- **Captures** (`~/.cache/kn-k2/cap/*-final2-trio.png`: mode 0 | Custom mode 3 all on | Cinematic).

## Settings, overrides, console, self-test

Render tab → Ray Tracing folder (Box Office-only rows, `client/src/ui/ui_client_settings.c`;
stored in the game store like vsync / windowMode; clamps in `client_apply_render_setting`):

| key | field (`ClientRenderSettings`) | values | default |
|---|---|---|---|
| `rtMode` | `rt_mode` | 0 off, 1 shadows, 2 + AO, 3 + reflections | 0 |
| `rtScale` | `rt_scale` | 0.25 / 0.5 / 0.75 / 1 of the scene resolution | 0.5 |
| `rtSamples` | `rt_samples` | AO rays per pixel 1 / 2 / 4 / 8 | 2 |
| `rtDenoise` | `rt_denoise` | temporal + spatial denoiser | on |
| `rtSunRadius` | `rt_sun_radius` | sun angular radius, degrees (0 = hard) .. 2 | 0.5 |
| `rtLocalLights` | `rt_local_lights` | shadow rays to the point / spot lights | off |
| `rtTaa` | `rt_taa` | temporal anti-aliasing of the scene (any `rtMode`; at 0 it runs the RT prepass for the motion vectors) | off |
| `rtBloom` | `rt_bloom` | bloom of emissive and over-bright surfaces (`rtMode` ≥ 1; half-float scene target) | off |
| `rtShafts` | `rt_shafts` | volumetric sun shafts through the map's fog (`rtMode` ≥ 1) | off |
| `rtSharpen` | `rt_sharpen` | 0 off, 1 low, 2 medium, 3 high: edge-adaptive upscale + sharpening instead of the bilinear stretch (`rtMode` ≥ 1) | 0 |

Environment (dev / headless): `KRUNKNATIVE_RT=<0-3>` and `KRUNKNATIVE_RT_SCALE=<f>` override the
store for the run, `KRUNKNATIVE_RT_SHAFTS=1` turns on `rtShafts`, `KRUNKNATIVE_RT_SHARPEN=<0-3>` sets `rtSharpen`; `KRUNKNATIVE_RT_DEBUG=<bvh|normals|visibility|history|ao|local|refl|motion|gi>`
is the console's `rt_debug`; `KRUNKNATIVE_RT_FORCE_REFL=1` (or `=<roughness>`, 0.05 = mirror)
makes every lit material reflect so the effect can be looked at on maps without reflective content;
`KRUNKNATIVE_RT_REFL_DEBUG=<1..6>` shows the hit albedo / normal / length / hit-miss / material
colour / atlas code; `KRUNKNATIVE_RT_ATLAS_DUMP=<out.png>` writes the atlas after each update;
`KRUNKNATIVE_RT_NO_MOTION=1` drops the velocity target (camera-only reprojection, for A/B); `KRUNKNATIVE_RT_NO_GRADIENT=1` turns off the denoisers' temporal-gradient clip (A/B);
`KRUNKNATIVE_RT_NO_EMISSIVE_LIGHTS=1` empties the emissive light list (`KRUNKNATIVE_RT_EMISSIVE_LOG=1` logs it);
`KRUNKNATIVE_RT_NO_GI=1`, `KRUNKNATIVE_RT_NO_SKY_AMBIENT=1` and `KRUNKNATIVE_RT_NO_SELF=1` leave GI, the
sky ambient and the self-shadow puppet off for A/B captures; `KRUNKNATIVE_RT_ALPHA_CAST=1` makes
alpha-tested map objects cast (Krunker's chain-link cubes do not), for testing the atlas alpha path;
`KRUNKNATIVE_RT_NO_MATERIALS=1`, `KRUNKNATIVE_RT_NO_MASKS=1` and `KRUNKNATIVE_RT_NO_VIEWMODEL=1` leave the
material classes, the weapon masks and the viewmodel pass off; `KRUNKNATIVE_RT_MATERIALS_LOG=1` logs every
class resolution; `KRUNKNATIVE_RT_NO_FILMIC=1` keeps the map's None / Linear curve instead of the filmic shoulder; `KRUNKNATIVE_RT_MASK_DUMP=<dir>` writes the built weapon masks; `KRUNKNATIVE_RT_NO_BUMP=1`, `KRUNKNATIVE_RT_NO_BEVEL=1`, `KRUNKNATIVE_RT_NO_FILTER=1` and `KRUNKNATIVE_RT_NO_SPEC_OCCLUSION=1` leave the relief normal maps, the bevels, the texture filtering and the specular occlusion off; `KRUNKNATIVE_RT_BUMP_BLUR=<0..1>` tunes the relief height blur; `KRUNKNATIVE_RT_SKY_VIS=1` turns on the (over-budget) world sky-visibility ray at mode 1. `KRUNKNATIVE_RT_NO_FLASH_LIGHT=1` adds no muzzle-flash / explosion lights. `KRUNKNATIVE_RT_NO_WATER=1` keeps Krunker's alpha blend on water and glass.

Console (developer commands): `rt_debug <bvh|normals|visibility|history|ao|local|refl|motion|gi|off>`
blits that intermediate over the frame; `rt_mode [0-3]` shows or sets `rtMode` through the settings
store so the Settings row follows; `rt_materials_reload` re-reads the material table. `KRUNKNATIVE_GPU_TIMING=1` prints the `rt` pass (GPU and CPU).

Self-test: `client_bin --offline --rt-selftest <map.json>` (headless) builds the scene over the
loaded map's world scene and compares the BVH tracer against brute force: 20 000 random rays
through the map box + 2 000 shadow rays from sun-found surfaces (closest hit: same instance and t;
occlusion with the caster / transparent filter), timings (first build, 20 steady-state rebuilds,
100 000 traces), then a perturbation pass (20 meshes nudged into the dynamic half, the biggest
geometry's positions moved → refit, 5 000 more rays, 61 still frames and a check that the map
objects settled back) and a skinned check (`models/pets/pet_19.glb` posed at 0.7 s: the refit
boxes hold the triangles). Exit 0 iff nothing mismatched. Run it on Sandstorm, Kanji and Burg
after touching rt_scene.c, rt_gpu.c or the traversal.

## Measured cost (RTX 2070 SUPER, release, headless, RT pass GPU time)

### GPU optimization pass (2026-11-03)

`KRUNKNATIVE_RT_TIMING=1` (desktop only) prints, once a second, the average GPU time of each world sub-pass of
`rt_frame`: `upload prepass trace reflect gi denoise refl_dn gi_dn water shafts` (GL timestamp queries in
rt_pass.c, read back a few frames late like gpu_timer.c). Measured with it, the passes are bound by each
pixel's chain of sequential rays, not by how many pixels trace: skipping half the pixels (per-pixel or per-tile
checkerboards, regular or random) saved only ~20 %, while removing a ray from every pixel's chain saved its
full share. The changes, each with a dev knob that restores the old path for A/B:

- **AO split over two frames** (`rt_ao_split`, rt_trace.frag): each frame traces half the `rtSamples` rays per
  pixel (rounded up) in fresh directions; the temporal history (up to 32 frames on the static world) adds them
  up. Only with the denoiser on and `rtSamples` ≥ 2. AO debug view and frames match the full trace within 2 %.
  `KRUNKNATIVE_RT_AO_FULL=1` traces every ray every frame.
- **Sun visibility at GI and reflection hits from the sun shadow map** (`shadow_sun_map`, shadow.c;
  `map_sun` in rt_gi.frag / rt_reflect.frag): one texture fetch instead of a second ray in the chain, for the
  world and the viewmodel. The map holds the static map casters only (no players), right for bounce light and
  the hit shading of a reflection. Without an active map (shadows off): the world GI reuses this frame's
  trace output where the hit lands on screen (`screen_sun`), else it traces the ray as before.
  `KRUNKNATIVE_RT_HIT_SUN_RAY=1` keeps the ray, `KRUNKNATIVE_RT_GI_TRACE_SUN=1` turns the screen reuse off.
- **Emissive shadow ray skipped for negligible lights** (rt_gi.frag `emissive_direct`): the light pick never
  comes up empty while any light is listed, so every pixel shot a shadow ray even at a sign across the map;
  below 2 % of the ambient the sample is added unshadowed, without the ray. No knob (the threshold keeps it
  below what a pixel can show).
- Tried and dropped: capping the GI ray's reach (no measurable gain), resampling 16 of Kanji's 64 lights per
  pixel (0.09 ms for visibly more variance), shadow-map visibility in the sun shafts (would lose your own body's
  shadow in the shafts for ~0.1 ms).

RT pass GPU ms, release, 1920x1080 window at the default resolution 0.6 / `rtScale` 0.5 (Cinematic: its own
0.6 / 0.4), pinned Sandstorm view `-77,140,-347`, medians of 3 runs under the shared GPU lock, load 3-8. The
"after" column also includes the other optimization forks' changes landing in the same tree (traversal in
rt_common.glsl, prepass culling); the per-change savings are from paired A/B runs:

| config | before | after | trace | GI | denoise |
|---|---|---|---|---|---|
| Sandstorm mode 1 | 0.53 | 0.44 | 0.22 | – | 0.16 |
| Sandstorm mode 2 | 6.05 | 2.92 | 1.93 → 1.13 | 3.07 → 1.32 | 0.74 → 0.28 |
| Sandstorm mode 3 | 5.95 | 2.10 | 1.94 → 0.76 | 2.30 → 0.74 | 0.77 → 0.24 |
| Sandstorm Cinematic | 3.99 | 1.73 | 1.46 → 0.57 | 1.62 → 0.57 | 0.29 → 0.17 |
| Kanji mode 2 | 4.05 | 2.27 | 1.49 → 0.68 | 1.85 → 1.15 | 0.41 → 0.27 |
| Kanji Cinematic | 3.76 | 1.84 | 1.10 → 0.50 | 1.24 → 0.76 | 0.50 → 0.18 |

Paired A/B savings (Sandstorm mode 2): AO split 0.85 ms (trace 1.97 → 1.13), shadow-map hit visibility 0.44 ms
(GI 2.37 → 1.93), emissive cutoff 0.6 ms (GI 1.93 → 1.32); reflection hits 0.54 → 0.17 ms at mode 3.
The table below predates this pass.


Traversal rewrite (2026-11-03, `rt_tlas_trace` single stack + child-slot half-float nodes): GPU `rt` column at a
1920x1080 window, resolution 0.6, rtScale 0.5, pinned pose, median of 3 locked runs, interleaved A/B (load 4-10):

| map | mode | before ms | after ms | change |
|---|---|---|---|---|
| Sandstorm | 1 | 0.53 | 0.48 | -10 % |
| Sandstorm | 2 | 4.89 | 3.34 | -32 % |
| Sandstorm | 3 | 5.08 | 3.48 | -32 % |
| Sandstorm | Cinematic | 3.85 | 2.68 | -30 % |
| Kanji | 1 | 0.77 | 0.64 | -17 % |
| Kanji | 2 | 3.90 | 3.15 | -19 % |
| Kanji | 3 | 3.93 | 3.02 | -23 % |
| Kanji | Cinematic | 3.10 | 2.36 | -24 % |

The older rows below predate it (and other passes added since); treat them as history.

| scene | rt_scale | effect | ms |
|---|---|---|---|
| 1920x1080 | 0.5 | mode 1 (sun shadows) | 1.0-1.15 |
| 1920x1080 | 1.0 | mode 1 | 3.7 |
| 1920x1080 | 0.5 | mode 2, 2 samples | 2.2-2.7 |
| 1920x1080 | 0.5 | mode 2, 4 samples | 3.8 |
| 1920x1080 | 0.5 | mode 3, no reflective material on screen | = mode 2 (early-out) |
| 1920x1080 | 0.5 | mode 3, every pixel forced reflective (worst case) | +4.4 (mirror) / +9 (roughness 0.25) |
| 1920x1080 | 0.5 | + local lights (6 in the map, 3 reaching the floor) | +0.9-1.0 (+0.3 in the world pass) |
| 1920x1080 | 0.5 | muzzle-flash light while firing continuously, mode 1 + `rtLocalLights` (load 10-30: upper bounds) | +0.4 RT pass, +0.3 world pass |
| 1152x648 | 0.5 | + local lights | +0.5 |
| 1920x1080 | 0.5 | motion-vector target | +0.02 |
| 1920x1080 | 0.5 | mode 2 + GI (half RT resolution), Kanji / Sandstorm | +1.1 / +1.6 (3.7 / 4.3 total; full RT resolution would be 6.0 / 7.1) |
| 1920x1080 | 0.5 | emissive lights in the GI pass, Kanji at a billboard (load 9-35: upper bounds) | +0.75 GPU, +0.05 CPU |
| 1920x1080 | any | sky ambient | 0 (a different ambient function in basic.frag) |
| 1920x1080 | 0.5 | first-person self shadow (separate puppet subtree, sun rays only; was ≈ 3.4 ms when every ray traversed it) | ≈ 0.05 |
| 1920x1080 | 0.5 | material classes (the GGX branch in the world pass) | +0.03 |
| 1920x1080 | 0.5 | viewmodel pass, mode 2 / mode 3 without a reflective weapon (measured under load: upper bounds) | +0.5 / +0.6 |
| 1920x1080 | 0.5 | viewmodel pass, every weapon pixel forced reflective (dev worst case) | ~3.5 |
| – | – | weapon mask built at run time (once per model, lazily) | 0.13-0.53 ms each on the GPU path, plus a one-time 33-47 ms shader compile on the first mask; ~12 ms each on the CPU fallback |
| 1920x1080 | – | TAA resolve (`post`) / velocity-only prepass at rtMode 0 (`rt`) | 0.14 / 0.28 |
| 1920x1080 | – | bloom chain + HDR composite (`post`, minus a plain scene target's 0.01-0.06), measured at load 17-28 | 0.2-0.3, up to 0.7 under load |
| 1920x1080 | – | `rtSharpen` upscale + sharpen (`post`), measured only at load 30-56: on and off were indistinguishable within that noise (2.2-2.6 ms either way) | not measured cleanly |
| 1920x1080 | – | filmic shoulder (world pass) | within noise |
| 1920x1080 window, scene resolution 0.6 | 0.5 | sun shafts: march + temporal (`rt`) / composite (`vfx` column) | within the load noise of ±1.5 ms at load 7-13 (12 steps at RT / 2 cost +3-5 ms, hence RT / 4 and 10 steps) / 0.04 |
| – | – | relief normal map, built once per texture | 0.5-2 ms CPU each (6.6-25.6 ms per map load incl. the first shader compile) |
| 1920x1080 | – | relief normal maps (world pass) | +0.04 |
| 1920x1080 | – | bevelled cube edges (world pass, measured under load) | +0.13 |
| 1920x1080 window, scene resolution 0.6 | 0.5 | water / glass at mode 3, the Oasis pool covering half the screen (medians of 25 samples a side under load 6-17): water reflection prepass + trace at RT / 3 (`rt`) / copy + composite (world pass) | +0.4 / +0.65 (before the RT / 3 divisor the reflection alone was about +7 ms) |
| 1920x1080 window, scene resolution 0.6 | 0.5 | water / glass at mode 1 (copy + composite, world pass; one run under load) | +0.06 |
| – | – | texture filtering | 0 per frame (a cache walk when the cache grows) |
| 1920x1080 | – | viewmodel sky probe (48 CPU rays) | +0.04 CPU, 0 GPU |
| 1920x1080 | 0.5 | world sky-visibility ray at mode 1 (opt-in, off by default) | +1.7 |
| 768x432 | 0.5 / 1.0 | mode 1 | 0.36 / 0.75 |

The world pass itself is 0.13-0.3 ms; the base frame on these maps is GPU-bound at 500-700 fps
(doc/perf-audit.md). Budget guidance in the setting texts: 2 AO samples is the 1080p budget, 4
roughly doubles the AO cost; local lights are about 1 ms more at 1080p; reflections cost only where
reflective materials are on screen.

## Documented differences and limits

- Water and glass: the refraction is a screen-space offset of the opaque frame (nothing off screen or
  hidden behind other transparent surfaces is refracted); the water reflection prepass draws the liquid
  plane flat (the waves only wobble the lookup); a single water / glass layer is composited (a pane seen
  through another sees the first one's alpha blend); more than `RT_WATER_MAX` visible water / glass meshes
  fall back to the sky reflection for the rest.
- Only the cube prefab takes the analytic path; prefab.c builds planes and ramps as their own
  triangle geometries (the `RT_INST_PLANE` / `RT_INST_RAMP` kinds are implemented and verified but
  unused until prefab.c shares the unit geometries).
- Mesh-UV geometry (OBJ / glTF) reflects its texture's mean colour, not the texel; Krunker cube /
  plane UVs in reflections ignore repeat / offset / rotation / flips.
- Alpha-tested materials (cut-out fences, foliage) occlude shadow / AO / local rays by their atlas
  alpha only where the atlas has their texture and the hit has a cube / plane UV; triangle
  alpha-test instances without a UV path are opaque to rays.
- Rect-area and directional map lights are not shadowed by rays; the spot penumbra is not in the
  traced shadow; at most 8 local lights, 4 traced per frame.
- Reflections: one bounce; mirrors-everywhere maps cost 5-10 ms at 1080p/0.5; faint fireflies at
  mirror silhouettes (history rejection there); the ocean keeps its planar mirror (`ocean.c`).
- The denoiser's history is depth / normal / velocity validated, not material validated: a texture
  swap under a still camera is not a disocclusion.
- GI is one bounce at half the RT resolution, bilinearly upsampled (slight bleeding across depth
  edges possible); most of Kanji's neon signs are plain textured, non-emissive materials and do not
  bleed (content). The sky ambient is subtle because v10's sky gradient is fog-coloured over most
  of the hemisphere.
- The self-shadow puppet is a separate BVH subtree (`RtScene.self_tlas_root`, −1 without one, the
  `rt_self_root` uniform) that the main TLAS never links to: only the sun shadow ray pushes it as a second start node; AO, GI,
  local-light and reflection rays (the viewmodel's too) skip it (`rt_skip_shadow_only`), so your own body does not
  occlude AO / GI (Krunker has no self-AO either). It exists only in first person (Krunker builds no body for the first-person
  model); the shadow-map path (`rtMode` 0) still has no self shadow, as in Krunker. The puppet has
  no pet and never joins a scene.
- TAA: the viewmodel has no velocity and is held by the neighbourhood clamp alone (camera-only
  reprojection); distant detail softens slightly; TAA forces the scene target even at resolution 1
  with no MSAA. No `rt_debug taa` view.
- `rt_supported()` needs a complete RGBA16F colour attachment (core on desktop; `EXT_color_buffer_float`
  on WebGL2 / ANGLE, which all current browsers and ANGLE expose) and `GL_MAX_TEXTURE_SIZE` ≥ 2048;
  otherwise the tracer logs once and `rtMode` behaves as 0. ANGLE (ES 3.0) renders every mode
  within ~1 % of the desktop frame at 5 % fuzz (temporal noise); the web build compiles the shaders
  (ES 3.00: no `f` suffixes, bounded loops, fixed stacks of 24, `RT_MAX_STEPS` 2048).
- Material classes are heuristic content, not measured materials: one class per texture family,
  weapon classes chosen by eye, masks from colour rules (a black-painted metal part and a black
  rubber grip look alike: the overrides file fixes the visible ones). Melee skins above id 51 default
  to metal unchecked. The table applies only while ray tracing is on.
- The first-person weapon is not in the BVH: it never reflects or shadows itself and casts no traced
  shadow on the world; its reflections and GI come from the world only. No `rt_debug` view shows the
  viewmodel's intermediates.
- `GPU_TIMESTAMP` queries are desktop-only, so the `rt` GPU column reads 0 on ANGLE / web.

## Verifying a change

`make` (re-run the CMake configure when adding files under `client/src/rt/`: the Makefile skips
it), `./cmake-build-debug/proc_input_test`, the self-test on the three maps, a pinned-camera
headless capture at `rtMode 0` before and after compared with `compare -metric AE` (mask the
animated billboard / grass regions, which differ between any two runs), captures at the modes you
touched looked at with the Read tool, `KRUNKNATIVE_GPU_TIMING=1` on the release build, and the
ANGLE configuration (`-DKRUNKNATIVE_GL_BACKEND=angle -DANGLE_DIR=...`) for ES validity.
