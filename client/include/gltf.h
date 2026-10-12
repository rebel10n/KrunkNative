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
// Not supported (the model loads without them, with a warning): EXT_meshopt_compression and
// other required compression extensions (the model fails to load), EXT_texture_webp images,
// non-triangle primitives, PBR metal/roughness/normal/occlusion maps (BasicMaterial is
// Lambert/Phong like the rest of the scene), KHR_materials_* extensions. Env maps (skin `env`)
// are a BasicMaterial feature still to come.
#include <client.h>

typedef struct GltfModel GltfModel;
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
    double last_now;   // gltf_instance_update's previous `now`, < 0 before the first call
} GltfInstance;

// Loads (or returns the cached) model at `path` (a full path, e.g. under assets_path()).
// NULL when the file is missing or cannot be used. Models stay cached for the process.
GltfModel *gltf_model_load(const char *path);
int gltf_model_animated(const GltfModel*); // has animations (clips)

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
// Set every mesh's visibility.
void gltf_instance_set_visible(GltfInstance*, int visible);
void scene_add_gltf(Scene*, const GltfInstance*);
void scene_remove_gltf(Scene*, const GltfInstance*);
// Frees the meshes, their materials and the instance's GL buffers; the model stays cached.
void gltf_instance_fini(GltfInstance*);
