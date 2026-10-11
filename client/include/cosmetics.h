#pragma once
#include <stddef.h>

// Krunker v3.1.1 store.skins entries that dress the player model (jLrR6): hats, bodies (back),
// dyes, waists and faces. The table (cosmetics_table.c) is generated from src/data/store.js.
typedef enum {
    COSMETIC_HAT = 1,   // models/hats/hat_<id>
    COSMETIC_BACK = 2,  // models/body/body_<id>
    COSMETIC_DYE = 5,   // recolours skin / shirt / sleeve / pants / shoes / hair
    COSMETIC_WAIST = 6, // models/waist/waist_<id>
    COSMETIC_FACE = 7,  // models/faces/face_<id>
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
    unsigned char no_head:1;     // hat replaces the head
    unsigned char no_legs:1;     // waist item replaces the legs
    unsigned char transparent:1; // opac != null || trans
} Cosmetic;

extern const Cosmetic g_cosmetics[];
extern const size_t g_cosmetic_count;

// The entry for a store index when it is of `type`; NULL for -1, unknown indices and other types.
const Cosmetic *cosmetic_get(int store_index, CosmeticType type);
