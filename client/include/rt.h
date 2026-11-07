#pragma once
// Ray tracing (Krunker Box Office-only, opt-in; doc/raytracing.md, history in doc/raytracing-plan.md). Portable GPU ray tracing in
// GLSL fragment shaders over a BVH stored in float textures (texelFetch): no compute, no SSBO, no
// extension, so it runs on desktop GL 4.5, ANGLE/Metal and WebGL2 alike (client/include/gl.h rules).
//
// Modules (client/src/rt/):
//   rt_scene.c    CPU acceleration structure: a BVH per triangle geometry (BLAS, cached by Geometry),
//                 one instance per visible world mesh, a top-level BVH over the instances (TLAS),
//                 the material table; a CPU reference tracer (self-test, debugging).
//   rt_gpu.c      the RGBA32F tables (nodes, triangles, instances, materials) on the GPU.
//   rt_pass.c     the per-frame passes: depth + normal + material + velocity prepass at RT resolution,
//                 the trace pass (sun shadow / AO / local lights), the reflect pass, the GI pass, the denoisers
//                 (temporal reprojection + a-trous, rt_denoise / rt_refl_denoise / rt_gi_denoise.frag), rt_debug
//                 blits; with rtTaa and rt_mode 0 the prepass alone (motion vectors for render_target.c's TAA).
//   rt_atlas.c    the world textures' atlas and the reflection material rules.
//   rt_self.c     the local player's first-person shadow proxy (RT_INST_SHADOW_ONLY casters).
//   rt_materials.c the material class table (assets/materials/rt_materials.json): metalness / roughness /
//                 reflect per paint, weapon, model, texture or player part (RT_EFFECT_MATERIALS).
//   rt_masks.c    per-weapon metal / roughness part masks (built from the weapon texture at run time, or a
//                 hand-painted textures/<model>_mr.png), bound as the weapon's metal_rough_texture.
//   rt_normals.c  relief normal maps generated from the base textures (rt_bump.frag), per-class `bump`.
//   rt_filtering.c trilinear + anisotropic minification on cached world textures while rt_mode > 0.
// rt_pass.c also runs the viewmodel pass (rt_viewmodel_frame: GI / reflections for the first-person weapon, the
// directional sky probe for its specular occlusion) and the volumetric sun shafts (rt_shafts*.frag, rtShafts);
// render_target.c owns bloom (rt_bloom.frag, rtBloom) and the TAA resolve.
// basic.frag reads the denoised results on RT_TEXTURE_UNIT.. by gl_FragCoord (rt_effects bits: 1 shadows, 2 AO,
// 4 reflections, 8 local lights, 16 force-refl dev, 32 GI, 64 sky ambient, 128 material classes, 1024 bevels,
// 8192 sky visibility, 16384 sky probe; doc/raytracing.md has the table).
//
// With rt_mode 0 (the default) nothing here runs and the frame is pixel-identical to Krunker's.
#include <client.h>

// ---------------------------------------------------------------- settings (render_settings.h rt_*)

enum { RT_MODE_OFF = 0, RT_MODE_SHADOWS = 1, RT_MODE_AO = 2, RT_MODE_REFLECTIONS = 3 };
// basic.frag `rt_effects` bit mask (rt_pass.c rt_frame: mode 1 = shadows + sky ambient, mode 2 adds AO + GI,
// mode 3 adds reflections; local lights from rt_local_lights)
#define RT_EFFECT_SHADOWS      1
#define RT_EFFECT_AO           2
#define RT_EFFECT_REFLECTIONS  4
#define RT_EFFECT_LOCAL_LIGHTS 8
// Sky-dome ambient: basic.frag's constant AmbientLight irradiance becomes hemispherical irradiance from
// the map's sky (env_map.h's PMREM table, roughest level, sampled along the normal; sky_color / fog without a
// dome), energy-matched on luminance for an upward normal (basic.c sky_ambient_scale). Set with any rt_mode > 0;
// KRUNKNATIVE_RT_NO_SKY_AMBIENT=1 (dev) leaves it off for A/B captures. 32 is RT_EFFECT_GI.
#define RT_EFFECT_SKY_AMBIENT  64

#define RT_TEXTURE_UNIT 14        // basic.frag `rt_result` (BASIC_WEAR_TEXTURE_UNIT is 13)
#define RT_LOCAL_TEXTURE_UNIT 15  // basic.frag `rt_local` (local light visibility, phase 5)

// Phase 5, local light shadows (rt_local_lights setting, RT_EFFECT_LOCAL_LIGHTS): the trace pass also
// shoots one shadow ray per pixel to each of up to RT_LOCAL_MAX point / spot lights of basic.frag's
// forward list (the nearest to the camera; basic_lights_list gives the slot order), jittered over
// RT_LOCAL_LIGHT_RADIUS so the edges soften, and writes their visibilities to one RGBA32F target,
// two lights per channel at 12 bits each: channel = floor(v0 * 4095 + 0.5) * 4096 + floor(v1 * 4095 + 0.5)
// (exact in a float; nearest-sampled, basic.frag unpacks and filters by hand). The denoiser's temporal
// pass blends them per light through the same packing; no spatial filter (the lights are small).
// Rect-area and directional map lights stay unshadowed (documented difference).
#define RT_LOCAL_MAX 8
#define RT_LOCAL_LIGHT_RADIUS 0.5f // world units: the jitter disk of a local light's shadow ray
#define RT_LOCAL_RANGE_CAP 2000.0f // a light with distance 0 (infinite) is traced this far
// Cost controls: a light whose attenuated irradiance at the pixel (basic.frag's distance / decay /
// cone math times N.L, over PI) is below RT_LOCAL_MIN is not traced (visibility 1: it would not show);
// with the denoiser on, at most RT_LOCAL_PER_FRAME of the traced slots are shot per frame (a window
// rotating with the frame index, rt_local_mask) and the temporal pass holds the others' history.
#define RT_LOCAL_PER_FRAME 4
#define RT_LOCAL_MIN (1.0f / 255.0f)

// ---------------------------------------------------------------- muzzle flash / explosion lights (vfx.c)
// Box Office-only, rt_mode >= 1 and the muzzleFlash setting (explosions: rt_mode >= 1 alone): a shot puts a
// brief warm point light just ahead of the muzzle (the first-person barrel tip, else the shooter's eye
// RT_FLASH_FORWARD units along the aim), an explosion an orange one at its centre. They are forward lights
// (basic.c BasicLight.transient = 1): basic.frag lights the world and the first-person weapon with them, and
// with rtLocalLights the tracer shadows them, every frame, with no temporal history (select_local_lights puts
// them after the map's lights so the map lights keep their traced slots and histories). Intensity peaks on the
// first rendered frame and falls linearly to 0 over its life. KRUNKNATIVE_RT_NO_FLASH_LIGHT=1 (dev) turns them off.
#define RT_FLASH_MAX 4              // live flash / explosion lights (each takes a forward light slot)
#define RT_FLASH_TRACE_MAX 2        // of those, at most this many get RT shadow rays (the newest)
#define RT_FLASH_COLOR 0xffcc88     // warm muzzle light
#define RT_FLASH_INTENSITY 1.8f     // peak intensity (BasicLight units: x PI x light_unit_factor like map lights)
#define RT_FLASH_RANGE 45.0f        // cutoff distance, Krunker units (lights a room; (1 - d / range)^2 falloff)
#define RT_FLASH_LIFE_MS 60.0f
#define RT_FLASH_FORWARD 1.0f       // the light sits this far ahead of the barrel tip / the eye, along the aim
#define RT_FLASH_PHYS_REF 6.0f      // physical-units maps (correctLights): match the legacy brightness at this distance
#define RT_EXPLOSION_COLOR 0xff9944
#define RT_EXPLOSION_INTENSITY 3.0f
#define RT_EXPLOSION_RANGE 45.0f    // at least; scaled up with the blast radius
#define RT_EXPLOSION_LIFE_MS 160.0f

// ---------------------------------------------------------------- GPU table layout (rt_gpu.c, rt_*.frag)
//
// Every table is one RGBA32F 2D texture of width RT_TEX_WIDTH; element i of a table with T texels
// per element sits at texel (i * T) -> (x = idx % RT_TEX_WIDTH, y = idx / RT_TEX_WIDTH). Integers
// are stored as floats (exact below 2^24).
#define RT_TEX_WIDTH 2048

// BVH node on the GPU, RT_NODE_TEXELS texels of an RGBA32UI table (BLAS and TLAS share the layout; rt_gpu.c put_node).
// It holds its two CHILDREN's boxes, not its own, so traversal enters a node without testing it and tests both children
// per 2 fetches. One texel per child slot (left t0, right t1):
//   x = half2(min.x, min.y)  y = half2(min.z, max.x)  z = half2(max.y, max.z)   (packHalf2x16 order: first = low 16 bits;
//       min rounded down and max rounded up to the next half, so a box never shrinks)
//   w = RT_SLOT_INNER | node index   an inner child (node 0 is the root)
//     = first << RT_SLOT_COUNT_BITS | count   a leaf child: its first primitive (global) and primitive count
//       (count 0: an empty slot, never entered; first < 2^20, count < 2^11)
// A CPU leaf node (RtNode a < 0: a = -(first) - 1, b = count, as rt_scene.c builds it) is entered only as a root (a tiny
// TLAS, the self-shadow subtree, a one-leaf BLAS): it packs itself as its left slot and an empty right slot.
// The node indices are the CPU tree's (plus a BLAS's base), so the partial uploads by node range still hold.
// BLAS primitives are triangles; TLAS primitives are instances.
#define RT_NODE_TEXELS 2
#define RT_SLOT_INNER 0x80000000u
#define RT_SLOT_COUNT_BITS 11
// Triangle, RT_TRI_TEXELS texels, in the geometry's local space:
//   t0 = (v0.xyz, 0)  t1 = (v1.xyz, 0)  t2 = (v2.xyz, 0)   (w spare: per-triangle material / uv in phase 4)
#define RT_TRI_TEXELS 3
// Instance, RT_INST_TEXELS texels:
//   t0..t2 = world_to_local rows (3x4, row-major)   t3..t5 = local_to_world rows
//   t6 = (kind, blas_root_node, material, flags)    t7 = (aabb_min.xyz, 0)    (the TLAS holds the bounds; t7 spare)
#define RT_INST_TEXELS 8
// Material, RT_MAT_TEXELS texels:
//   t0 = (color.rgb, opacity)   t1 = (emissive.rgb, alpha_test)
//   t2 = (atlas slot + 1 + 4096 * uv_mode, uv_size.xyz; 0 = no texture: rt_atlas.c)   t3 = (metalness, roughness, reflect_intensity, unlit)
#define RT_MAT_TEXELS 4

// ---------------------------------------------------------------- CPU structures (rt_scene.c)

typedef enum {
    RT_INST_BOX = 0,       // g_cube_geometry: unit cube, analytic (local x,z in [-0.5, 0.5], y in [0, 1]: check loaders.c)
    RT_INST_PLANE = 1,     // g_plane_geometry: unit quad, analytic
    RT_INST_RAMP = 2,      // g_ramp_geometry: unit wedge, analytic
    RT_INST_TRIANGLES = 3, // any other geometry: a BLAS over Geometry.tri_positions / tri_indices
} RtInstanceKind;

#define RT_INST_CASTS_SHADOW 1   // occludes shadow / AO rays (shadow.c caster rules, players always)
#define RT_INST_DOUBLE_SIDED 2   // no back-face skip
#define RT_INST_ALPHA_TEST   4   // material alpha_test > 0: shadow / AO rays pass its cut-outs via the atlas (rt_common.glsl rt_alpha_passes)
#define RT_INST_TRANSPARENT  8   // material.transparent: never occludes, reflection rays pass through
#define RT_INST_DYNAMIC     16   // moved since the last frame / a player part (denoiser history off)
#define RT_INST_UNLIT       32   // emissive / unlit: reflections return its colour as-is
#define RT_INST_SHADOW_ONLY 64   // the local player's first-person shadow proxy (rt_self.c): occludes shadow / AO /
                                 // local-light rays, but the prepass never draws it and closest-hit rays (reflections,
                                 // GI) pass through it, so you cast a shadow without ever seeing your own body in a mirror

typedef struct {
    float world_to_local[12]; // 3x4 row-major
    float local_to_world[12];
    float aabb_min[3], aabb_max[3]; // world space
    int kind;                 // RtInstanceKind
    int blas;                 // RT_INST_TRIANGLES: index into RtScene.blas, else -1
    int material;             // index into RtScene.materials
    unsigned flags;           // RT_INST_*
    const Mesh *mesh;         // the scene mesh (not owned)
    // phase 6 motion vectors: the mesh's local_to_world of the previous rt_scene_build (equal to
    // local_to_world when it did not move); the prepass writes per-pixel velocity from both
    float prev_local_to_world[12];
} RtInstance;

typedef struct {
    float aabb_min[3], aabb_max[3];
    int a, b;                 // as in the texture layout: inner (left, right), leaf (-(first) - 1, count)
} RtNode;

typedef struct {
    const Geometry *geometry; // cache key (not owned); NULL = a dead entry (its geometry's triangle count changed:
                              // a new BLAS was built, this one's rows in RtScene.tris stay unused until the map unloads)
    RtNode *nodes;
    int node_count;
    int tri_first;            // first triangle of this BLAS in RtScene.tris
    int tri_count;
    int *tri_order;           // leaf order -> the geometry's triangle index (a refit re-reads the positions in this order)
    int source_tri_count;     // Geometry.index_count / 3 at build (a different count means a rebuild, not a refit)
    unsigned tri_epoch;       // Geometry.tri_epoch the triangles were read at
    unsigned refit_serial;    // bumped by every refit: rt_gpu re-uploads just this BLAS's node / triangle rows
} RtBlas;

typedef struct {
    float color[4];           // rgb + opacity
    float emissive[3];
    float alpha_test;
    float atlas[4];           // phase 4
    float metalness, roughness, reflect_intensity;
    int unlit;
} RtMaterial;

// Static / dynamic split (phase 6): instances [0, static_instance_count) are the map objects that have not
// moved for RT_STATIC_SETTLE_FRAMES frames, built once and kept (rebuilt when one of them moves, appears,
// disappears or changes geometry); the rest, [static_instance_count, instance_count), are rebuilt every
// frame (players, pets, builds, moving map objects). The TLAS is one table: node 0 is the root (an inner
// node over both halves), nodes [1, 1 + static_tlas_count) the static BVH, the dynamic BVH after it. An
// empty half is one leaf of count 0 with an inverted box (never hit). The traversal is unchanged.
#define RT_STATIC_SETTLE_FRAMES 60
typedef struct {
    RtInstance *instances; int instance_count, instance_cap;
    RtNode *tlas; int tlas_count, tlas_cap;
    RtBlas *blas; int blas_count, blas_cap;
    float *tris; int tri_count, tri_cap;         // 9 floats a triangle (local space), BLAS after BLAS
    RtMaterial *materials; int material_count, material_cap;
    unsigned static_epoch;    // bumps when tris / blas / materials changed (rt_gpu re-uploads those tables)
    unsigned frame_epoch;     // bumps every rebuild (instances + tlas re-upload)
    int skipped;              // visible meshes without usable geometry this frame (logged once)
    int static_instance_count;    // instances [0, n) and TLAS nodes [1, 1 + static_tlas_count) are the static half
    int static_tlas_count;
    unsigned static_tlas_epoch;   // bumps when the static half was rebuilt (rt_gpu re-uploads it; else only the dynamic rows)
    unsigned refit_epoch;         // bumps when some BLAS was refit this frame (rt_gpu re-uploads those BLAS's rows)
    int alpha_test_count;         // instances that cast and are alpha-tested (rt_pass binds the atlas to the trace pass only then)
    // The local player's shadow puppet (rt_self.c, RT_INST_SHADOW_ONLY): its instances are the last ones, its BVH the last
    // TLAS nodes, and node 0 does NOT link to it. Only the sun shadow ray
    // pushes this root as a second start node (rt_common.glsl rt_self_root uniform, rt_pass.c): AO / GI / local-light /
    // reflection rays never traverse the ~40 boxes around the camera (that cost ~3 ms of the RT pass). -1 = none.
    int self_tlas_root;
} RtScene;

typedef struct {
    float t;
    int instance;             // -1 = miss
    int triangle;             // local to the BLAS, -1 for analytic kinds
    float normal[3];          // world space, facing the ray
    float position[3];        // world space
} RtHit;

// Rebuilds the instance list and the TLAS from the visible meshes of `scene` (the world scene;
// the first-person scene is never traced), building/caching a BLAS for every triangle geometry
// it has not seen. Cheap enough to run every frame (~2k instances). sun_dir etc. are not needed
// here; the pass reads g_render_lighting.
// `shadow_only` (NULL = none): meshes traced as RT_INST_SHADOW_ONLY casters on top of the scene's (the local
// player's first-person shadow proxy, rt_self.c); their `visible` flags are respected like the scene's.
void rt_scene_build(RtScene *rt, const Scene *scene, const Scene *shadow_only);
// CPU reference tracer over the same data the GPU gets: closest hit / any hit (occlusion).
// Used by `client_bin --rt-selftest` and rt_debug; not per frame.
int rt_scene_trace(const RtScene *rt, const float origin[3], const float dir[3], float t_max, RtHit *hit);
int rt_scene_occluded(const RtScene *rt, const float origin[3], const float dir[3], float t_max);
// Brute force over every instance (no BVH): the self-test's oracle.
int rt_scene_trace_brute(const RtScene *rt, const float origin[3], const float dir[3], float t_max, RtHit *hit);
void rt_scene_free(RtScene *rt);
// Drops the BLAS cache entries of geometries that are gone (map unload); the rest of the scene is rebuilt each frame.
void rt_scene_unload_map(RtScene *rt);

// ---------------------------------------------------------------- GPU tables (rt_gpu.c)

typedef struct {
    unsigned int texture;
    int rows;                 // allocated height (RT_TEX_WIDTH wide)
    float *data;              // the packed table as uploaded (whole rows), kept so a partial re-upload of some rows
    size_t data_cap;          // has the right neighbours in them; floats
    int texels;               // texels in use
    unsigned char valid;      // the texture holds `data` (0 after a (re)allocation: the next upload is a full one)
    unsigned char integer;    // RGBA32UI (data holds uint bit patterns: the BVH node tables) instead of RGBA32F
} RtTable;

typedef struct {
    RtTable tlas, blas, tris, inst, mats;
    int tlas_count;           // nodes in the uploaded TLAS (0 = nothing to trace)
    int *blas_base;           // node base of every BLAS in the packed `blas` table
    unsigned *blas_serial;    // RtBlas.refit_serial as uploaded, per BLAS
    int blas_base_cap;
    unsigned uploaded_static_tlas_epoch, uploaded_refit_epoch;
    int blas_node_total;
    unsigned uploaded_static_epoch, uploaded_frame_epoch;
    unsigned char uploaded_static, uploaded_frame, failed_logged;
} RtGpu;

// Uploads what changed since the last call (static tables on static_epoch, TLAS + instances on
// frame_epoch). 0 when the tables do not fit the GPU's textures (logged once).
int rt_gpu_upload(RtGpu *gpu, const RtScene *rt);
// Binds the five tables on texture units first_unit .. first_unit + 4 and sets the rt_common.glsl
// samplers + rt_tlas_count of `program` (which must be in use).
void rt_gpu_bind(const RtGpu *gpu, unsigned int program, int first_unit);
void rt_gpu_free(RtGpu *gpu);

// ---------------------------------------------------------------- per-frame driver (rt_pass.c)

// main.c world pass, after sky_render() and the hide / third-person swaps, right before the world scene_render(): with rt_mode > 0
// rebuilds the scene, uploads, runs prepass + trace + denoise into the RT targets. Restores framebuffer,
// viewport, program, VAO, cull / depth / blend state like shadow_update(). GPU_PASS_RT.
void rt_frame(const Scene *scene, Camera *camera);
// Binds the RT result textures on their units and sets basic.frag's rt_effects / rt_size uniforms for
// the world scene draw; rt_end_world() clears rt_effects so the viewmodel / previews do not read them.
void rt_begin_world(void);
void rt_end_world(void);
// the first-person viewmodel: main.c calls rt_viewmodel_frame(client->fps_scene, &fps_camera) right before
// the fps scene draw (after rt_end_world): with rt_mode >= RT_MODE_AO and a full trace this frame it runs the
// prepass over the fps scene's meshes with the fps camera and the GI (mode >= 2) / reflect (mode 3) passes from
// those surfaces into the world's tables (the weapon is not in the BVH: it reflects the room, never itself), into
// targets of its own. rt_begin_viewmodel / rt_end_viewmodel bracket the fps draw like the world's pair: the fps
// draw gets RT_EFFECT_REFLECTIONS / RT_EFFECT_GI plus the world's material-level bits (sky ambient, materials),
// never the sun shadow / AO / local light bits (the viewmodel keeps the shadow map). KRUNKNATIVE_RT_NO_VIEWMODEL=1
// (dev) skips it for A/B.
void rt_viewmodel_frame(const Scene *fps_scene, Camera *fps_camera);
void rt_begin_viewmodel(void);
void rt_end_viewmodel(void);
// 1 while main.c draws the fps scene (between rt_begin_viewmodel / rt_end_viewmodel), whatever rt_mode: basic.c
// scales the viewmodel's bloom weight by RT_VM_BLOOM_SCALE.
int rt_viewmodel_drawing(void);
// The first-person items sit a unit from the camera and fill a large part of the screen, so a glowing one (the
// Purple Drank's emissive drink) bloomed into a screen-sized white blob that hid the item. Their bloom weight is
// scaled down so they glow like a light at that size instead of whiting out.
#define RT_VM_BLOOM_SCALE 0.2f
// The RT_EFFECT_* bits of the draw in progress: the world's between rt_begin_world / rt_end_world, the
// viewmodel's between rt_begin_viewmodel / rt_end_viewmodel, else 0 (the tracer off, HUD / previews):
// basic.c hides Krunker's ambient_0/1 AO planes while RT_EFFECT_AO is set.
int rt_world_effects(void);
// TAA (render_target.c, rtTaa): this frame's motion vectors from the prepass (RT resolution; .xy = uv delta
// current - previous, unjittered, .w = 1 where a world surface was drawn) and the camera-only reprojection
// for the rest (sky, viewmodel): previous_view_projection * inverse(view_projection), both unjittered,
// row-major. 0 = no velocity this frame (rt_frame did not run its prepass). With rtMode 0 and rtTaa on,
// rt_frame runs the BVH-less velocity-only prepass (one depth + MRT draw of the world at RT resolution).
unsigned int rt_taa_velocity(int *width, int *height, float sky_reproject[16]);
// Bumps whenever the RT history was reset (map change, resize): render_target.c drops the TAA history too.
unsigned int rt_history_epoch(void);
// Per-draw, from scene_render() next to shadow_apply_mesh_uniforms(): basic.frag's rt_effects for
// this mesh (0 outside rt_begin_world / rt_end_world, for transparent meshes and for programs
// without the uniform) plus the result sampler / viewport mapping.
void rt_apply_mesh_uniforms(const Mesh *mesh);
// rt_debug <bvh|normals|visibility|ao|history|local|refl|motion|off>: blits the named intermediate over the frame (console).
void rt_debug_set(const char *what);
// The local player's own shadow in first person (rt_self.c): Krunker builds no body for the first-person model
// (only arms), so the tracer keeps a third-person "shadow puppet" of `me` (player_generate_meshes(.., 0) on a copy
// of the player's state, posed every frame by player_update_meshes like another player's model) and traces it as
// RT_INST_SHADOW_ONLY. main.c calls this each world pass before rt_frame: enabled 0 (dead, spectating, the own
// model already drawn in third person) or me NULL = no proxy this frame. Only acts with rt_mode > 0.
void rt_set_self_caster(Player *me, int enabled);
// rt_frame: the puppet's meshes for rt_scene_build (NULL = none); builds / re-poses the puppet first.
const Scene *rt_self_frame(void);
void rt_self_unload_map(void); // the puppet's meshes go with the map's geometry / weapons
void rt_self_fini(void);
void rt_unload_map(void);  // map switch: history reset, BLAS cache drop
void rt_fini(void);
// 1 when the context can run the tracer (float colour targets, texture size); logged once otherwise.
int rt_supported(void);
// rt_supported()'s answer without any GL call: 0 until the first rt_frame checked the context (settings code).
int rt_supported_cached(void);
// The look-first Cinematic preset (rtPreset 1, doc/raytracing.md "Cinematic preset"): 1 when it is selected, the
// stored rtMode is > 0 and the tracer is supported. It implies rtMode 3's effects, rtLocalLights / rtTaa / rtBloom /
// rtSky / rtShafts on, and for the rows still at their default rtScale RT_CINE_RT_SCALE, rtSamples 2, rtSharpen Low
// and resolution RT_CINE_RESOLUTION (rows the player changed keep their value: the Ray Traced settings preset has
// them at their maximum; applied through client_render_settings(), never written to the store), and gates the look
// changes (deep shadows, sun, GI / AO / exposure balance, grading, surface detail strength) that break Krunker's
// brightness on purpose.
int rt_cinematic(void);
// `client_bin --rt-selftest <map.json>` (main.c, once the offline map is loaded and drawn): builds the
// scene over `scene` (the world scene), fires random rays, BVH vs brute force; returns the exit code.
int rt_selftest(const Scene *scene, const char *map_path);

// ---------------------------------------------------------------- reflections: the texture atlas (rt_atlas.c, phase 4)
//
// RtMaterial.atlas = (slot + 1 + RT_ATLAS_UV_MODE_STEP * uv_mode, uv_size.xyz), 0 = no texture: the material's
// base texture copied into cell `slot` of one RT_ATLAS_SIZE RGBA8 atlas (RT_ATLAS_CELL px cells, drawn with the
// GPU's mip filter), uv_mode 1 = Krunker cube UVs, 2 = plane UVs, 0 = the mesh's own (not in the triangle table:
// such hits use the cell's mean colour, texture rt_atlas_mean[slot]).
#define RT_ATLAS_SIZE 2048
#define RT_ATLAS_CELL 128
#define RT_ATLAS_COLS (RT_ATLAS_SIZE / RT_ATLAS_CELL)
#define RT_ATLAS_SLOTS (RT_ATLAS_COLS * RT_ATLAS_COLS)
#define RT_ATLAS_UV_MODE_STEP 4096
#define RT_REFL_TEXTURE_UNIT 16  // basic.frag `rt_refl` (the denoised reflection radiance)
#define RT_EFFECT_FORCE_REFL 16  // KRUNKNATIVE_RT_FORCE_REFL (dev): basic.frag blends rt_refl into every lit surface

// ---------------------------------------------------------------- one-bounce diffuse GI (rt_gi.frag, rt_gi_denoise.frag)
// RT_EFFECT_GI (rt_mode >= RT_MODE_AO): every lit pixel shoots one cosine-weighted ray; its radiance L estimates
// the bounce irradiance G = PI * L. Krunker's maps are lit for a strong constant ambient A, and one physical
// bounce off ~0.4-albedo walls returns far less than A indoors (a third of the frame's brightness was lost),
// so G does not replace A: basic.frag (krunker_gi_irradiance) keeps A's luminance as a floor, takes G's colour
// as a tint of strength RT_GI_TINT (normalised so the tint alone never changes the luminance), and lets G
// raise the level only where it exceeds A (a sunlit floor's bounce, an emissive sign):
//   E = A * t / lum(A * t) * lum(A) * max(1, lum(G) / lum(A)),   t = mix(1, (G / lum(G)) / (A / lum(A)), RT_GI_TINT)
// then AO once. Maps without ambient get E = G. Walls take colour from the ground, unlit / emissive signs
// light their surroundings, sunlit bounce brightens shadowed areas. The result (denoised: temporal + 3 a-trous steps) is read on RT_GI_TEXTURE_UNIT (an 18th fragment
// sampler: unavailable on a GL with 16, logged once). KRUNKNATIVE_RT_NO_GI=1 (dev) leaves it off for an A/B.
#define RT_EFFECT_GI 32
#define RT_GI_TEXTURE_UNIT 17   // basic.frag `rt_gi`
#define RT_GI_TINT 0.6f         // how much of the bounce's colour tints the ambient (0 = brightness only, 1 = the bounce's hue)
#define RT_GI_EMISSIVE 1.0f       // emissive / unlit radiance at a GI hit (the neon bleed strength)
#define RT_GI_DIVISOR 2           // the GI ray and its denoiser run at RT resolution / this (bounce light is low frequency;
                                  // basic.frag's bilinear sample upsamples it; full RT resolution cost 2x the budget)
#define RT_VM_DIVISOR 2           // the first-person viewmodel's RT target set: the world's RT resolution / this (rt_pass.c)
#define RT_GI_SKY_MIX 0.5f        // a miss: how far the ambient-equivalent radiance leans to the sky colour (0 = Krunker's ambient)

// ---------------------------------------------------------------- volumetric sun shafts (rtShafts; rt_pass.c, doc/raytracing.md)
// rt_shafts.frag marches every view ray, one sun shadow ray per step, through a thin exponential haze of density
// RT_SHAFTS_DENSITY / fog_far (Krunker's own THREE.Fog, smoothstep(fog_near, fog_far, d), is almost nothing
// within 100 units, so it alone would only put shafts in the distance); rt_shafts_composite adds sun light *
// RT_SHAFTS_STRENGTH * phase * (lit haze share) and removes RT_SHAFTS_SHADOW of the map's fog that lies in shadow.
// Any rtMode > 0 with the rtShafts switch.
#define RT_SHAFTS_DIVISOR 4       // the march and its temporal pass run at RT resolution / this (shafts are low frequency; the composite upsamples by depth)
#define RT_SHAFTS_DENSITY 4.0f   // the scattering haze reaches 1 - e^-4 = 98 % at the fog's far distance, 47 % at a sixth of it
#define RT_SHAFTS_STRENGTH 0.35f  // in-scattered sun light per unit of lit fog toward the sun (phase 1 + iso)
#define RT_SHAFTS_SHADOW 0.4f     // share of the map's shadowed fog colour taken back out (0 = additive shafts only)
#define RT_SHAFTS_G 0.6f          // Henyey-Greenstein asymmetry: forward scattering, brightest looking at the sun
#define RT_SHAFTS_ISO 0.1f        // isotropic floor of the phase (normalised to 1 toward the sun)
#define RT_SHAFTS_HAZE 0.3f       // a map without fog: the strength of the default haze (additive only)
#define RT_SHAFTS_HAZE_FAR 1500.0f // ... and its far distance (fog_near 1)
// main.c, after the world draw and its effects, before the first-person scene: composites this frame's shafts
// into the bound framebuffer (no-op unless rt_frame marched them). Restores blend / depth / cull / units 0-1.
void rt_shafts_composite(void);

// Fills RtMaterial.atlas and the reflection rules (metal/roughness of a skin env material, the forced
// reflections) from the BasicMaterial; called by rt_scene.c's material_index before the dedupe.
void rt_atlas_material(RtMaterial *m, const BasicMaterial *bm);
// Draws the slots assigned since the last call (needs a bound GL context; `vao` is a fullscreen-triangle VAO).
void rt_atlas_update(unsigned int vao);
// Binds the atlas and the mean-colour texture on two units and sets `program`'s rt_atlas / rt_atlas_mean / rt_atlas_ready.
void rt_atlas_bind(unsigned int program, int atlas_unit, int mean_unit);
int rt_atlas_count(void);              // textures in the atlas (log / stats)
int rt_atlas_force_reflections(void);  // KRUNKNATIVE_RT_FORCE_REFL set
void rt_atlas_unload_map(void);        // the map's textures are gone: slots are reassigned
void rt_atlas_fini(void);

// ---------------------------------------------------------------- material classes (rt_materials.c, assets/materials/rt_materials.json)
// Box Office physical materials for content that has none (Krunker's models and map textures are flat
// colour + one texture): a hand-authored table maps paint shaders, weapon ids, model paths, texture base
// names and player parts to a class (metalness, roughness, reflect). Resolved per BasicMaterial on demand
// (rt_materials_resolve: basic.c before a lit draw, rt_atlas_material for the tracer's table) and cached
// on the material (BasicMaterial.rt_class, re-resolved when its texture / paint changes). Applied only
// while RT_EFFECT_MATERIALS is in rt_effects (any rt_mode > 0): basic.frag then shades the material with
// the GGX model (pbr_active) using the class factors, whatever the lighting setting, and the reflect pass
// traces it when reflect > 0. rt_mode 0: nothing resolves, nothing changes. A glTF material with its own
// metallicRoughness map keeps it (no class) unless the map is a Box Office part mask (BasicMaterial.rt_mask).
#define RT_EFFECT_MATERIALS 128

// ---------------------------------------------------------------- bevelled cube edges (shading only)
//
// Krunker's map boxes (prefab_build_cube: unit cube scaled per axis) have razor-sharp edges. With
// RT_EFFECT_BEVEL (any rt_mode > 0; KRUNKNATIVE_RT_NO_BEVEL=1 dev A/B) basic.frag bends the shading
// normal of a cube face toward its neighbour face within RT_BEVEL_WIDTH world units of an edge (up to
// the 45-degree bisector at the edge itself), so edges catch a thin highlight from the sun / GGX and
// darken on the shadow side. Shading only: the RT prepass, the BVH and collisions keep the hard box.
// The width shrinks to RT_BEVEL_FACE_FRACTION of the face's shorter side on small faces (a 1-unit plank
// keeps a visible flat face). An edge against a face the map masked out ("f": the box touches another
// object there) is not bevelled. Skipped: transparent, alpha-tested, unlit, sprite and non-cube meshes,
// and the first-person scene (rt_viewmodel_frame never sets the bit).
#define RT_EFFECT_BEVEL 1024
#define RT_BEVEL_WIDTH 0.6f
#define RT_BEVEL_FACE_FRACTION 0.25f

// ---------------------------------------------------------------- specular occlusion of the sky reflection
//
// A classed material without a traced reflection reflects the sky (basic.frag krunker_rt_mat_specular), which
// is wrong under a roof: a metal under an overhang showed open sky. The sky term is scaled by a visibility,
// and the occluded part is replaced by the radiance of nearby shaded surfaces (half the flat ambient, or the
// pixel's GI bounce when GI runs), so it goes warm-dark like the traced reflection rather than black:
//   - rtMode >= 2: the traced AO through Lagarde's specular occlusion, so(NdotV, ao, roughness)
//   - rtMode 1, world (RT_EFFECT_SKY_VIS, opt-in with KRUNKNATIVE_RT_SKY_VIS=1): the trace pass spends one ray
//     in a cosine lobe about the mirror/normal halfway, at most RT_SKY_VIS_RADIUS long, into rt_result.g (the
//     AO channel is unused at mode 1), on one 8x8 tile of each 16x16 block a frame (rt_denoise.frag holds
//     the history of the rest). Off by default: it cost ~+1.7 ms at 1080p / 0.5 on the RTX 2070 SUPER (+0.9 ms
//     at an 8-unit reach), far over the ~0.3 ms such a fix should cost; without it the world's classed metal
//     at mode 1 keeps the unoccluded sky (mode 2+ is occluded by the traced AO, mode 3 traces reflections)
//   - the first-person viewmodel, any mode (RT_EFFECT_SKY_PROBE): the weapon is camera-locked and small, so
//     one "ambient cube" of six sky visibilities from the camera (+-x, +-y, +-z; RT_SKY_PROBE_RAYS CPU rays
//     each in a cosine lobe, closest hit stepping past transparent instances (the ambient strips lie on every
//     floor) and never into the self-shadow puppet, smoothed over frames by
//     RT_SKY_PROBE_BLEND), evaluated per pixel along the reflection direction (uniforms rt_sky_probe_pos /
//     rt_sky_probe_neg). A single upper-hemisphere value was tried first and failed: a weapon mostly mirrors
//     near-horizontal directions (walls), not the sky straight above.
// KRUNKNATIVE_RT_NO_SPEC_OCCLUSION=1 (dev) leaves all three off for A/B captures. Bits 8192 / 16384 leave the
// low values to the other effects.
#define RT_EFFECT_SKY_VIS 8192
#define RT_EFFECT_SKY_PROBE 16384
#define RT_SKY_VIS_RADIUS 60.0f
#define RT_SKY_PROBE_RAYS 8 // per axis (RT_SKY_PROBE_PER_FRAME axes a frame)
#define RT_SKY_PROBE_BLEND 0.15f
#define RT_SKY_PROBE_PER_FRAME 2 // axes traced a frame, round robin (16 CPU rays instead of 48; same time constant)

typedef struct {
    char name[32];
    float metalness;   // 0..1
    float roughness;   // 0..1
    float reflect;     // 0..4: the tracer's reflect_intensity (0 = no reflection ray)
    float f0;          // 0..1: a metal's minimum specular colour ("f0" in the table; default 0.45 for classes with
                       // metalness >= 0.5, else 0). Krunker's weapon textures are often black paint, and a metal's
                       // F0 is its albedo, so without it a black-textured gun reflects nothing; with it the metal
                       // reads as gunmetal grey tinted by whatever the texture lightens: F0 = mix(0.04, max(albedo, f0), metalness)
    float bump;        // 0..2: relief strength of the normal map generated from the texture ("bump"; absent = 0; rt_normals.c)
    int detail;        // RT_DETAIL_*: the Cinematic preset's procedural micro-detail on map cubes ("detail": "stone" /
                       // "metal" / "wood"; absent = none; basic.frag krunker_rt_detail)
} RtMaterialClass;
enum { RT_DETAIL_NONE = 0, RT_DETAIL_STONE = 1, RT_DETAIL_METAL = 2, RT_DETAIL_WOOD = 3 };

enum { RT_PART_NONE = 0, RT_PART_BODY, RT_PART_HEAD, RT_PART_ARMS, RT_PART_LEGS, RT_PART_COSMETIC };

typedef struct {
    const char *texture;   // texture base name (no directory / extension), NULL = none
    const char *model;     // model path relative to assets ("models/x.obj", "weapons/..."), NULL = none
    int weapon;            // weapon id, -1 = none
    int part;              // RT_PART_*
    int paint;             // paintShader 0..5, -1 = none
} RtMaterialKey;

// Precedence: paint > weapons > models > textures > parts > default (globs: `*` only). NULL = no class.
const RtMaterialClass *rt_materials_lookup(const RtMaterialKey *key);
int rt_materials_load(void);     // (re)reads assets/materials/rt_materials.json; 1 on success (logs the counts)
int rt_materials_loaded(void);   // a table with at least one class is in memory
// Resolves `material`'s class from its texture (the texture cache's path), hints (rt_hint_*) and paint; cheap
// when nothing changed since the last call. Sets rt_class / rt_metalness / rt_roughness / rt_reflect.
void rt_materials_resolve(BasicMaterial *material);
// Hints for the parts of the key the material cannot tell on its own; set where the mesh is built.
void rt_materials_hint_model(BasicMaterial *material, const char *model_path);
void rt_materials_hint_weapon(BasicMaterial *material, int weapon_id);
void rt_materials_hint_part(BasicMaterial *material, int part);

// ---------------------------------------------------------------- per-weapon masks (rt_masks.c)
// A metallicRoughness map (G rough, B metal) per weapon / melee MODEL, bound as the material's
// metal_rough_texture while rtMode > 0 so the class factors (rt_materials) are shaped per part (barrel metal,
// grip wood / rubber). Built at run time from the weapon's base texture on the GPU (rt_mask_classify.frag /
// rt_mask_filter.frag; CPU fallback), with tools/rt-materials/weapon_masks.py's rules and the overrides in
// assets/materials/weapon_masks.json; a "textures/<model key>_mr.png" next to the texture is a hand-painted
// override. Weapon and melee keys only. rtMode 0: nothing.
unsigned int rt_mask_texture(const char *model_key);              // "weapons/weapon_2", "melee/melee_0"; 0 = none
void rt_mask_apply(BasicMaterial *material, const char *model_key); // sets metal_rough_texture (+ unit factors)
void rt_masks_fini(void);

// ---------------------------------------------------------------- generated normal maps (rt_normals.c)
// Surface relief for Krunker's flat textures while ray tracing is on: a tangent-space normal map built once per
// base GL texture on the GPU (assets/shaders/rt_bump.frag: height = the texture's luminance, smoothly upsampled
// 1-4x so pixel-art steps become soft bevels, a Sobel gradient at RT_BUMP_SLOPE), bound by basic.c as the
// material's normal map (BASIC_NORMAL_TEXTURE_UNIT, normal_scale = the class's `bump`) for classed materials
// under RT_EFFECT_MATERIALS that have no normal map of their own. basic.frag's krunker_perturb_normal (screen-
// space derivatives, no tangents) applies it on whatever UV the base texture is sampled with (cube uv modes,
// repeats, flips, rotation). Built lazily: a material asks during the draw, the map is queued and built at the
// next rt_frame (one frame without relief), so no GL state changes inside scene_render. The RT prepass keeps the
// geometric normal (rays leave from the real surface). Nothing here runs with rtMode 0.
// Dev knob: KRUNKNATIVE_RT_NO_BUMP=1.
#define RT_BUMP_SLOPE 3.0f       // tangent-space slope per unit luminance step per source texel (normal_scale 1)
#define RT_BUMP_FINE_MAX 512     // the map is built at up to 4x the texture, until its longer side reaches this
unsigned int rt_normals_material(BasicMaterial *material); // the material's generated map, 0 = none (yet)
void rt_normals_frame(void);      // rt_frame: builds the queued maps (restores the GL state it touches)
void rt_normals_unload_map(void); // drops every map (the materials' caches follow through the epoch)
void rt_normals_fini(void);

// ---------------------------------------------------------------- texture filtering (rt_filtering.c)
// Krunker minifies its map textures with NearestFilter and no anisotropy (deob-10.0.6.js:159855-159856), so far
// floors shimmer. While rtMode > 0 every texture of the texture cache minifies trilinearly with anisotropic
// filtering up to RT_FILTER_ANISOTROPY (where EXT_texture_filter_anisotropic exists); the mag filter stays nearest.
// rtMode 0 restores each texture's own min filter and anisotropy 1 (pixel-identical to Krunker's).
#define RT_FILTER_ANISOTROPY 8.0f
void rt_filtering_update(int want); // rt_frame, every frame: want = rtMode > 0 and the tracer supported; KRUNKNATIVE_RT_NO_FILTER=1 (dev) keeps Krunker's
void rt_filtering_fini(void);

// ---------------------------------------------------------------- emissive lights (GI next-event estimation)
// Neon signs and other emissive surfaces light their surroundings directly: rt_scene_emissive_lights picks, every
// frame, the up to RT_EMISSIVE_MAX instances that emit (RT_INST_UNLIT: Krunker's MeshBasicMaterial signs and
// light bars, or a non-zero emissive) with the largest estimated contribution near the camera, each as an
// oriented box (its local bounds through local_to_world) and a radiance = the hit radiance rt_gi.frag gives the
// same instance (material colour x the texture's atlas mean + emissive). rt_gi.frag samples one of them per pixel
// (importance power x area / distance^2), a point on a box face that faces the pixel, one shadow ray, and adds
// Le cos cos / d^2 x area / pdf to the bounce; a bounce ray that hits a listed light drops its emission so no light
// is counted twice (no MIS: documented in doc/raytracing.md). Part of RT_EFFECT_GI (rtMode >= 2);
// KRUNKNATIVE_RT_NO_EMISSIVE_LIGHTS=1 (dev) leaves the list empty for A/B captures.
// GPU table (rt_gpu_upload_emissive): one RGBA32F row, RT_EMISSIVE_TEXELS texels a light:
//   t0 = (centre.xyz, power = lum(radiance) x box area)   t1 = (half-axis 0, sum of the squared half-axes)
//   t2 = (half-axis 1, instance index)   t3 = (half-axis 2, 0)   t4 = (radiance.rgb, 0)
#define RT_EMISSIVE_MAX 64
#define RT_EMISSIVE_TEXELS 5
#define RT_EMISSIVE_UNIT 9            // rt_gi.frag `rt_emissive` during gi_pass (0..8 are the tables, depth, normal, atlas)
#define RT_EMISSIVE_LIGHT_SCALE 4.0f  // the emitted radiance relative to the colour the sign is drawn with (tuned on Kanji)
#define RT_EMISSIVE_MIN_LUM 0.08f     // dimmer emitters are left to the bounce
typedef struct {
    float center[3];
    float axis[3][3];         // half-extent vectors
    float radiance[3];
    int instance;             // global instance index (the bounce's hit.instance)
} RtEmissiveLight;
int rt_scene_emissive_lights(const RtScene *rt, const float camera[3], RtEmissiveLight *out, int max);
typedef struct {
    unsigned int texture;
    int count;
} RtEmissiveTable;
void rt_gpu_upload_emissive(RtEmissiveTable *table, const RtEmissiveLight *lights, int count);
void rt_gpu_free_emissive(RtEmissiveTable *table);
int rt_atlas_mean_rgb(int slot, float rgb[3]); // the atlas cell's mean colour (0..1), 0 when not drawn yet

// ---------------------------------------------------------------- water and glass (rt_pass.c, basic.frag)
//
// Transparent map liquids and glass get a fresnel blend of a reflection and a refraction of the scene behind them,
// instead of Krunker's plain alpha blend over it. Which materials: a transparent BasicMaterial whose class
// (rt_materials.json) is "water" (kind 1) or "glass" / "window" (kind 2), and every transparent Krunker LIQUID plane
// (prefab.c gives each a wave height; most are untextured colours that no texture rule can class, so the table's "water"
// class shades them; a glowing liquid, with an emissive colour, keeps Krunker's look). Opaque liquids keep the class
// shading of the opaque world (they are in the prepass and get the world's traced reflection already). rt_mode > 0.
// The sky reflection is occluded by the camera's sky probe (RT_EFFECT_SKY_PROBE, rt.h "specular occlusion").
//   Refraction: scene.c calls rt_water_copy_scene() once, between the opaque and the transparent draws of the world
//     scene; when a water / glass mesh is visible it blits the opaque frame (colour) into a texture. basic.frag reads it
//     at the fragment, offset by the wave normal's deviation from flat (water only: glass panes are flat and not
//     distorted), rejecting an offset that lands on something in front of the surface (the world prepass depth). Water
//     thickness (the prepass depth behind minus the fragment's) darkens the view through it toward the water colour:
//     body = max(authored opacity, 1 - exp(-thickness / RT_WATER_DEPTH)).
//   Reflection: rtMode 3 traces one ray per water / glass pixel at 1 / RT_WATER_DIVISOR of the RT resolution (a prepass
//     of up to RT_WATER_MAX of those meshes and the reflect + reflection denoise passes on a third target set, like the
//     viewmodel's; meshes past the cap fall back to the sky estimate), elsewhere the sky estimate of the material
//     sky-specular path. Fresnel: Schlick with F0 0.02 (water) / 0.04 (glass).
//   Composite: coverage w = F + (1 - F) x body; the surface colour (reflection and lit water body) is fogged and tone
//     mapped by basic.frag's usual tail, then out = surface x w + refracted x (1 - w) with alpha 1 (the shader does the
//     blend, since the refracted texel is not the one behind the fragment). Without a copy (none this frame): alpha w,
//     the hardware blend, no distortion.
// Effect bit RT_EFFECT_WATER on the water / glass draw only (with RT_EFFECT_SKY_AMBIENT / RT_EFFECT_MATERIALS; the
// per-pixel world bits stay off: their textures describe the opaque surface behind). Units during those draws:
// RT_WATER_SCENE_UNIT (the opaque copy; rt_result's unit, unread by water), RT_WATER_DEPTH_UNIT (the world prepass
// depth; rt_local's unit), RT_REFL_TEXTURE_UNIT (the water reflection). Transparent draws follow every opaque draw of
// the world scene, and rt_frame unbinds those units at the start of the next frame.
// KRUNKNATIVE_RT_NO_WATER=1 (dev) keeps Krunker's alpha blend for A/B captures.
#define RT_EFFECT_WATER 32768

// ---------------------------------------------------------------- Cinematic lighting (rt_cinematic, doc "Cinematic preset")
// RT_EFFECT_CINEMATIC on the world, viewmodel and water draws while the preset is active. basic.frag's
// krunker_lambert then lights for looks instead of Krunker's brightness: the indirect light is the directional sky
// ambient at RT_CINE_FILL of Krunker's level plus the GI bounce at its real brightness (no ambient floor), times AO;
// the sun is RT_CINE_SUN brighter and warmed toward the horizon; a per-map exposure (rt_pass.c cine_exposure, from
// the map's lighting, no eye adaptation) brings the frame back to a pleasing mean. rt_gi.frag's hits use the same
// fill and its misses the full sky; emissive light is RT_CINE_EMISSIVE stronger; AO reaches RT_CINE_AO_RADIUS.
#define RT_EFFECT_CINEMATIC 65536
#define RT_CINE_RESOLUTION 0.6f    // the scene resolution the preset implies (Krunker's default; TAA + rtSharpen Low keep it crisp). 0.75 cost 12.7 ms, 1.0 21 ms a frame at 1080p
#define RT_CINE_RT_SCALE 0.4f      // its RT scale: with 0.6 the whole frame is ~7.3 ms GPU at 1080p on a 2070 SUPER (0.5: 9.3 ms; the 8 ms budget)
#define RT_CINE_FILL 0.7f          // the flat / sky ambient kept, as a fraction of Krunker's
#define RT_CINE_SUN 1.1f           // the sun's direct light relative to the map's
#define RT_CINE_AO_RADIUS 14.0f    // AO ray reach (RT_AO_RADIUS 12 otherwise)
#define RT_CINE_EMISSIVE 2.0f      // the GI emissive scale relative to RT_GI_EMISSIVE
#define RT_CINE_GI_SKY_MIX 0.1f    // a GI miss: ambient -> sky (the physical sky's blue at full strength turned every shadow blue)
#define RT_CINE_EXPOSURE_BIAS 1.0f // the exposure on top of the brightness match (tuned on Sandstorm / Burg / Kanji)
#define RT_WATER_SCENE_UNIT RT_TEXTURE_UNIT
#define RT_WATER_DEPTH_UNIT RT_LOCAL_TEXTURE_UNIT
#define RT_WATER_DEPTH 24.0f       // Krunker units of water that turn the view through it mostly into the water colour
#define RT_WATER_REFRACT 0.6f      // screen-space offset per unit of wave-normal deviation (fraction of the view height)
#define RT_WATER_MAX 64            // water / glass meshes the reflection prepass draws per frame
#define RT_WATER_DIVISOR 3         // the water / glass reflection is traced at 1 / 3 of the RT resolution (cost, rt_pass.c)
// Between the world scene's opaque and transparent draws (scene.c): the opaque frame for the refraction.
void rt_water_copy_scene(const Scene *scene);

// ---------------------------------------------------------------- Cinematic colour pipeline (rt_grade.c)
//
// Only while rt_cinematic() (doc/raytracing.md "Cinematic preset" > "Colour pipeline"). Four pieces:
//   - tone curve: every lit draw (basic.frag krunker_tone_map, sky.frag, rt_sky.frag) runs three.js's ACES
//     fit at RT_CINE_EXPOSURE times the preset's exposure instead of the filmic shoulder when the map's own
//     curve is None / Linear (basic.frag: krunker_cine_curve; the sky passes: rt_grade_exposure());
//   - grading: a RT_GRADE_LUT_SIZE^3 RGBA8 3D LUT (built on the CPU when the map's mood changes), applied with a
//     vignette in the final composite (scene_composite.frag / rt_sharpen.frag via rt_grade.glsl), after bloom and
//     before the player's own saturation / hue filter. Day maps: S-curve contrast, cool shadows / warm
//     highlights, more saturation in the mid tones; night maps: the authored hue, deeper blacks, saturated neon.
//     The mix follows the physical sky's day factor (sky_physical_day);
//   - bloom: a display-luminance threshold on top of the HDR weight, wider and stronger (render_target.c);
//   - sky and shafts: a more saturated zenith, fuller clouds, a stronger aureole; denser, brighter shafts whose
//     colour and lobe follow the sky's sun.
// The Custom preset sees none of it (every uniform below is 0 / false outside rt_cinematic()).
#define RT_GRADE_LUT_SIZE 32
#define RT_GRADE_UNIT 2              // the composite's `grade_lut` sampler (unit 0 the scene, 1 bloom)
#define RT_GRADE_VIGNETTE 0.15f      // darkening at the frame's corners
#define RT_CINE_EXPOSURE 1.05f       // into the ACES fit (three's /0.6 included): 1.15 washed sand / neon to white, 0.95 ran ~25 % darker than Krunker
#define RT_CINE_BLOOM_STRENGTH 0.65f // render_target.c BLOOM_STRENGTH under Cinematic
#define RT_CINE_BLOOM_RADIUS 1.4f    // wider upsample tent than the Custom bloom
#define RT_CINE_BLOOM_KNEE 0.9f      // display luminance from which highlights bloom (the sun disk; lower let the whole sky glow)
#define RT_CINE_SPEC_BLOOM 0.35f    // basic.frag krunker_bloom_weight: non-emissive over-brightness blooms at this share under Cinematic (sun glints on metal stay highlights)
#define RT_CINE_SHAFTS_DENSITY 1.5f  // x RT_SHAFTS_DENSITY
#define RT_CINE_SHAFTS_GAIN 1.3f     // x RT_SHAFTS_STRENGTH (1.8 hazed the whole frame)
// The preset's per-map exposure from the lighting balance (rt_pass.c cine_exposure; basic.frag applies it as
// rt_cine_exposure before the curve); 1 outside the preset. The sky passes use RT_CINE_EXPOSURE * this.
float rt_cinematic_exposure(void);
float rt_grade_exposure(void);        // RT_CINE_EXPOSURE * rt_cinematic_exposure() * the map's linear exposure, 0 outside Cinematic
// render_target.c, before the final composite: rebuilds the LUT if the mood changed and binds it on
// RT_GRADE_UNIT; returns 1 when grading applies this frame (then the shader's `grade_on` is set).
int rt_grade_bind(void);
void rt_grade_fini(void);

// ---------------------------------------------------------------- radiance guards (basic.frag, rt_gi.frag, rt_reflect.frag)
// RT_RADIANCE_MAX: the reflect and GI passes clamp their output radiance and scrub NaN / Inf, so one bad sample
// cannot enter a temporal history or the bloom chain. RT_BLOOM_WEIGHT_MAX caps basic.frag's bloom weight (how far a
// colour goes above 1). The GLSL keeps its own copies of both values.
#define RT_RADIANCE_MAX 32.0f
#define RT_BLOOM_WEIGHT_MAX 4.0f
