#pragma once
#include <stddef.h>

// Krunker store.skins entries that dress the player model: hats, bodies (back), dyes, waists and
// faces, plus v7.1.4's HEAD and BACK items (v7 split them off HAT and BODY; their meshes still
// load from hats/ and body/, store.js assetType). The table (cosmetics_table.c) is generated from
// assets/skins.json by tools/ui-import/gen_cosmetics.py.
typedef enum {
    COSMETIC_HAT = 1,   // models/hats/hat_<id>
    COSMETIC_BACK = 2,  // models/body/body_<id>
    COSMETIC_DYE = 5,   // recolours skin / shirt / sleeve / pants / shoes / hair
    COSMETIC_WAIST = 6, // models/waist/waist_<id>
    COSMETIC_FACE = 7,  // models/faces/face_<id>
    COSMETIC_V7_BACK = 14, // v7 BACK: worn in the back slot, models/body/body_<id> (assetType 2)
    COSMETIC_V7_HEAD = 15, // v7 HEAD: worn in the hat slot, models/hats/hat_<id> (assetType 1)
} CosmeticType;

typedef struct {
    int index;          // store.skins index (what hatIndex / backIndex / ... hold)
    int type;           // CosmeticType
    int id;             // model id
    const char *name;
    int tex;            // texture variant (<model>_<tex>.png), -1 = the model's own texture
    float scl_mlt;      // sclMlt || 1
    float sit_off;      // sitOff || 0
    float sit_off_z;    // sitOffZ || 0
    float opacity;      // opac || 1
    float alpha_test;   // alpha || 0
    int double_sided;   // side == 2 (DoubleSide)
    int color;          // material colour 0xRRGGBB, -1 = none
    int skin_col, shirt_col, sleeve_col, pants_col, shoe_col, hair_col; // dyes, -1 = keep
    int asset_type;     // v7 store.js assetType (model directory type), 0 = `type` itself
    float anim_s;       // hasAnim: AnimationMixer timeScale (animS || 1)
    const char *file_ext; // hasAnim: model file extension (fileExt || "gltf"), NULL = .obj
    unsigned char no_head:1;     // hat replaces the head
    unsigned char no_legs:1;     // waist item replaces the legs
    unsigned char transparent:1; // opac != null || trans
    unsigned char has_anim:1;    // v7 hasAnim: an animated GLTF/GLB model (render.js loadAnimMesh, client/src/gltf.c)
} Cosmetic;

extern const Cosmetic g_cosmetics[];
extern const size_t g_cosmetic_count;

// The entry for a store index when it is worn in the slot of `type`; NULL for -1, unknown
// indices and other types. COSMETIC_HAT also accepts v7 HEAD items, COSMETIC_BACK v7 BACK items.
const Cosmetic *cosmetic_get(int store_index, CosmeticType type);
