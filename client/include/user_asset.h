#pragma once
// Krunker v7.1.4 user assets ("aid": CUSTOM_ASSET, TRIGGER, AI; MapManager.addAsset ->
// render.js loadMesh / loadAnimMesh) from assets/user-assets/<id>/: model.obj + model.png
// (OBJ) or model.gltf / model.glb (GLTF, animated), with the flag letters of the aid string
// (doc/v7/map-logic.md §1.5.4). client/src/user_asset.c.
#include <client.h>
#include <gltf.h>

typedef struct {
    char id[24];               // the digits of aid
    unsigned char emissive;    // e: model_e.png emissive map, emissive #ffffff
    unsigned char normal;      // n (not drawn: BasicMaterial has no normal maps)
    unsigned char specular;    // s (not drawn)
    unsigned char alpha;       // a: alphaTest 0.02
    unsigned char gltf, glb, fbx; // g / q / f: loadAnimMesh (FBX is not supported: no mesh)
    unsigned char metal, rough, displace, bump; // m / r / p / b (not drawn)
    unsigned char lightmap;    // l: model_l.png lightMap (intensity 1.5, uv2 = uv)
} UserAssetFlags;

// 0 when aid has no asset id digits.
int user_asset_parse(const char *aid, UserAssetFlags *out);

typedef struct {
    Mesh *mesh;       // OBJ: the geometry as loaded (Geometry.bound_center for loadMesh's re-centring),
                      // a BasicMaterial with model.png (+ the emissive / alpha / lightmap flags);
                      // the caller owns it (mesh_fini) and may change its material
    GltfModel *gltf;  // GLTF / GLB: the cached model, instanced by the caller
} UserAssetModel;

// Loads one asset. 0 when nothing could be loaded (missing files, FBX).
int user_asset_load(const UserAssetFlags *flags, UserAssetModel *out);

// The map-object mesh of a CUSTOM_ASSET / TRIGGER with "aid" (prefab.c). A GLTF asset returns
// an empty carrier Mesh that its instance hangs off (registered for user_asset_map_*).
Mesh *user_asset_object_mesh(Object *object, const cJSON *raw_obj, const vec4 *colors);

// GLTF map assets: add / remove their instances to / from the scene (map_post.c), and play
// their clips each frame (visibility follows the carrier mesh). The instances stay while their
// carriers do (a new round on the same map keeps the map's meshes): user_asset_mesh_freed drops them.
void user_asset_map_load(Scene *scene);
void user_asset_map_unload(Scene *scene);
// A map object's mesh is about to be freed (shared/src/map.c map_fini / map_object_free): its GLTF
// instance and any wait for its model go with it.
void user_asset_mesh_freed(const void *mesh);
// A map object built after the map loaded (a live asset, entities.c): the GLTF instance under its carrier joins the
// scene now (one still downloading joins when it arrives, user_asset_arrived).
void user_asset_object_added(const void *carrier);
void user_asset_frame(void);
// A downloaded file arrived (asset_fetch_client.c, after the loaders' caches were refreshed): a map
// object whose model was still downloading when the map was built gets it now (an OBJ is re-centred on
// the real geometry, a GLTF / GLB instance is created under its carrier and added to the scene).
void user_asset_arrived(const char *path);
// Game.playAnim on a GLTF map asset: the instance hanging off `carrier` plays `clip` reps times
// (render.js playClip). No-op for other meshes.
void user_asset_play_clip(const Mesh *carrier, const char *clip, int reps);
