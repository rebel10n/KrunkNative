#pragma once
// GLTF 2.0 / GLB models (client/src/gltf.c on client/lib/cgltf), for Krunker v7.1.4's animated
// store items (skins with hasAnim: RENDER.loadAnimMesh, v7 src/libs/render.js:63-215, 2231) and
// GLTF user assets. A model is parsed once per file and turned into the renderer's own
// structures: one Mesh (Geometry + BasicMaterial) per primitive, a MeshTransform per node
// (the node hierarchy), the GLTF materials mapped onto BasicMaterial (base colour factor and
// texture, emissive factor and texture, alpha mode/cutoff, double-sided). Embedded (data URI,
// GLB, bufferView) and external buffers and images are supported; every file is opened through
// ensure_asset_file() (web: fetched on demand), external images through load_texture().
//
// Animation (three.js AnimationMixer as loadAnimMesh drives it: every clip plays at once,
// looping, timeScale = animS): node translation / rotation / scale and morph target weights,
// LINEAR / STEP / CUBICSPLINE samplers. Morph targets and skins are evaluated on the CPU and
// re-uploaded per instance (the cosmetics are small), so BasicMaterial and the shadow pass need
// no skinning shader. Normals are not used: basic.frag derives flat normals from the geometry.
//
// EXT_meshopt_compression (v7's AI / map .glb user assets) is decoded at load
// (client/src/gltf_meshopt.c). Not supported (the model loads without them, with a warning): other
// required compression extensions (KHR_draco_mesh_compression: the model fails to load), EXT_texture_webp images,
// non-triangle primitives, PBR metal/roughness/normal/occlusion maps (BasicMaterial is
// Lambert/Phong like the rest of the scene), KHR_materials_* extensions. Env maps (skin `env`)
// are a BasicMaterial feature still to come.
#include <client.h>

typedef struct GltfModel GltfModel;

// Which clips gltf_instance_update plays (v7 render.js loadAnimMesh / playClip / stopAllClips)
enum { GLTF_CLIPS_ALL, GLTF_CLIPS_LOOP, GLTF_CLIPS_ONCE, GLTF_CLIPS_STOPPED };
typedef struct GltfInstanceNode GltfInstanceNode;
typedef struct GltfDynamicPrimitive GltfDynamicPrimitive;

typedef struct GltfInstance {
    // Place the model with root: parent it to an anchor and set position / rotation / scale
    // like any Mesh transform (loadAnimMesh's mesh: xP/yP/zP, rotation, scale).
    MeshTransform root;

    size_t mesh_count;
    Mesh **meshes; // owned: one per primitive (materials owned by the meshes)

    // private
    GltfModel *model;
    GltfInstanceNode *nodes;
    GltfDynamicPrimitive *dynamic;
    size_t dynamic_count;
    float time_scale;  // animS
    double time;       // seconds of animation played
    int clip_mode;     // GLTF_CLIPS_*
    int clip;          // the clip of GLTF_CLIPS_LOOP / _ONCE
    int clip_reps;     // GLTF_CLIPS_ONCE repetitions
    double clip_start; // `time` when that clip started
    double last_now;   // gltf_instance_update's previous `now`, < 0 before the first call
} GltfInstance;

// Loads (or returns the cached) model at `path` (a full path, e.g. under assets_path()).
// NULL when the file is missing or cannot be used. Models stay cached for the process.
GltfModel *gltf_model_load(const char *path);
int gltf_model_animated(const GltfModel*); // has animations (clips)
// The file behind `path` changed (a resource pack came or went, mod_pack.c): the next
// gltf_model_load(path) parses it again; instances keep the model they were made from.
void gltf_model_forget(const char *path);
void gltf_model_forget_all(void); // F5 (game_reload.c): every file model; not the viewer's imports
// The viewer's imports (v7 RENDER.updateMesh(key, data, "gltf" | "glb")): from now on
// gltf_model_load(path) answers the model parsed from `source`; the model `path` had stays alive
// for the instances that use it. 0 when `source` cannot be loaded (nothing changes).
// gltf_model_restore(path) brings the file's own model back (RENDER.reset()).
int gltf_model_override(const char *path, const char *source);
void gltf_model_restore(const char *path);

// A new copy of the model's scene: meshes, node transforms and animation state.
// time_scale: animS (1 = the clips' own speed). NULL on failure.
GltfInstance *gltf_instance_create(GltfModel*, float time_scale);
// Advance the clips to `now` (seconds, any monotonic clock: the first call starts at 0) and
// apply them: node transforms, morph weights, skins. Cheap no-op for unanimated models.
void gltf_instance_update(GltfInstance*, double now);
// The animation clock for gltf_instance_update: glfwGetTime(), or the pinned virtual time of
// a --render-scenario run (ui_scenario.c), so scenario captures are reproducible.
double gltf_clock(void);
void gltf_set_clock_override(double seconds); // < 0: back to glfwGetTime()
// KrunkScript GAME.SCENE (v7 createKrunkScriptApiContext.js:861-880, render.js:63-205):
#define GLTF_HAVE_SCRIPT_API 1
// getBone: the transform of the first skin joint named `name` (SkeletonUtils.getBoneByName),
// NULL when none. Writes to it last until the next sampled clip moves that node; skins follow it.
MeshTransform *gltf_instance_bone(GltfInstance*, const char *name);
// Every node's current transform (bones included), in the file's node order: tools/gltf_test.
size_t gltf_instance_node_count(const GltfInstance*);
const MeshTransform *gltf_instance_node(const GltfInstance*, size_t index);
// playClip(name, reps): reps > 0 plays that clip alone `reps` times and clamps on its last frame;
// reps 0 loops it (no restart when it already is the looping clip, and no effect while a reps > 0
// clip is still playing: v7's isRunning guard). Unknown names: no change.
void gltf_instance_play_clip(GltfInstance*, const char *clip, int reps);
// 1 while a reps > 0 clip plays (not yet clamped on its last frame).
#define GLTF_HAVE_CLIP_RUNNING 1
int gltf_instance_clip_running(const GltfInstance*);
// stopAllClips(): no clip plays; the nodes keep their last pose.
void gltf_instance_stop_clips(GltfInstance*);
// updateMorphKey(name, weight): weight clamped to 0..1 on every mesh whose morph target names
// (mesh extras.targetNames) contain `target_name`, applied at once.
void gltf_instance_set_morph(GltfInstance*, const char *target_name, float weight);
void gltf_instance_set_time_scale(GltfInstance*, float time_scale); // mixer.timeScale
// The current morph target weights of every node, in node order (up to `max` into out; NULL: count
// only). Returns how many the model has. (WP-F gltf_test, the viewer's inspector.)
int gltf_instance_morph_weights(const GltfInstance*, float *out, int max);
// Set every mesh's visibility.
void gltf_instance_set_visible(GltfInstance*, int visible);
void scene_add_gltf(Scene*, const GltfInstance*);
// A shared geometry that draws nothing (index_count 0): the carrier Mesh a GLTF model hangs off
// when existing code poses a single Mesh (player_mesh.c weapon / melee remodels). Never freed.
Geometry *gltf_empty_geometry(void);
void scene_remove_gltf(Scene*, const GltfInstance*);
// Frees the meshes, their materials and the instance's GL buffers; the model stays cached.
void gltf_instance_fini(GltfInstance*);
