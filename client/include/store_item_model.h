#pragma once
// A store item's model as RENDER.loadMesh / loadAnimMesh build it from its store entry (client/src/store_item_model.c):
// src store.types[assetType || type || 0] + (assetType || type ? id : weapon) + (mid != null ? "_" + mid : ""), texSrc
// (getReModel, melee/melee_<id>[_<tex>], <src>_<tex>, tex, weapons/skins/weapon_<weapon>_<id>), the OBJ or, for a
// hasAnim item, the GLTF / GLB, with the item's material options and animations (skin_fx.c). The viewer's showcase and
// cosmetic pages (viewer_scene.c) and the map prize wheels' won item (prize_wheel_render.c, deob-10.0.3.js:219527-219565)
// draw it.
#include <client.h>
#include <gltf.h>

struct ViewerSkin;

typedef struct {
    Mesh *mesh;          // the OBJ item (skin_fx applied), or NULL
    GltfInstance *model; // the hasAnim item, or NULL
} StoreItemModel;

// Builds the item under `parent` at `position`, turned rot_y about y and scaled `scale` (loadMesh's x, y, z, rotation,
// scale). glow_text / emissive: getMat's glowText / emissive; no_tex: noTex (no texture). Not added to any scene.
// 0 when the item has no model that loads.
int store_item_model_build(const struct ViewerSkin *skin, MeshTransform *parent, vec3 position, float rot_y, float scale, int glow_text,
                           int emissive, int no_tex, StoreItemModel *out);
// Frees what store_item_model_build made (its skin_fx registrations too); remove it from its scene first.
void store_item_model_fini(StoreItemModel *model);
