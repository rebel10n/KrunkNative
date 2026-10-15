#pragma once
// Krunker v10.0.2 store data (the v10 store module after load, v10 config): every store.skins entry
// (8069, the indices of assets/skins.json; 0-6447 are v7.1.4's, same items), rarities, types, previews, generated into
// viewer_store_table.c by tools/ui-import/gen_viewer_store.js. Read by the viewer page and the
// store / inventory / loadout / market UI. Client-only (the shared code and cosmetics_table.c keep
// their own subsets).
#include <stddef.h>

enum {
    VS_GLOW = 1 << 0,        // glow: emissive map "<tex>_e" (or the texture itself with sameGlow)
    VS_SAME_GLOW = 1 << 1,
    VS_NO_EMIS = 1 << 2,
    VS_TRANS = 1 << 3,
    VS_WIRE = 1 << 4,
    VS_BLOCKED = 1 << 5,
    VS_IS_3D = 1 << 6,
    VS_FLIP = 1 << 7,
    VS_DOUBLE_SIDE = 1 << 8,
    VS_LOW_R = 1 << 9,
    VS_NO_HEAD = 1 << 10,
    VS_NO_LEGS = 1 << 11,
    VS_PAT = 1 << 12,        // pat != null: `tex` is a full texture path (melee patterns)
    VS_TEX_NUM = 1 << 13,    // tex is a number (texture variant "<model>_<tex>"), else a path
    VS_RGB = 1 << 14,
    VS_NO_SPIN = 1 << 15,
    VS_NOT_IN_WHEEL = 1 << 16, // limited / blackM / opensea / limT / limG: never in a prize wheel's pool (getSpnItems)
    VS_FREE = 1 << 17,       // free: labelled "[Free]" instead of its rarity (v7 viewer lists)
    VS_HAS_ANIM = 1 << 18,   // hasAnim: an animated GLTF model "<src>.<file_ext || gltf>" (render.js loadAnimMesh)
};

typedef struct ViewerSkin {
    const char *name;
    int type;            // itemTypes: 0 weapon skin, 1 hat, 2 body, 3 melee, 4 spray, 5 dye, 6 waist, 7 face,
                         // 8 shoe, 9 pet, 10 collectible, 11 wrist, 12 charm, 13 ticket, 14 back, 15 head, 16 player card,
                         // v10: 17 emoji, 18 paint
    int weapon;          // weapon skins: 1-based weapon index (game.weapons[weapon - 1]), else 0
    int id;              // model / skin id, -1 when absent (or not a number: id_name)
    int rarity;          // store.rarities index
    int seas;            // season, 0 = absent (season 1)
    const char *tex;     // tex as text (number or path), NULL when absent
    int pat;             // pattern id, -1 when absent
    int mid;             // remodel id, -1 when absent
    int mid_t;           // remodel texture id, -1 when absent
    unsigned flags;      // VS_*
    float x_off, y_off, z_off, m_scl, scl, spn_scl, x_rot, z_rota, scl_mlt, sit_off, sit_off_z;
    float sx_off, sy_off, sz_off, sx_rot, alpha, prev_rot, tile_s;
    float opac;          // opac (0 = absent)
    float opacity;       // sprays' opacity (0 = absent)
    int emiss;           // emissive colour, -1 = white
    int color;           // material colour, -1 = none
    int side;            // three.js side (2 = DoubleSide)
    int frames, frame_t; // sprite-sheet frames
    float r_anim;        // rAnim: spin period, 0 = none
    char r_anim_axis;    // 'x' / 'y' / 'z'
    float mov_t, puls_t;
    const char *creator; // skin.creator, NULL = "Krunker.io"
    int asset_type;      // assetType: the type whose model / preview directory it uses (BACK 14 -> 2, HEAD 15 -> 1), 0 = type
    const char *tex_ext; // texExt (player cards, emoji: "webp"), NULL = "png"
    const char *prev_ext; // prevExt: the preview's own extension (v10), NULL = tex_ext
    const char *file_ext; // fileExt of a hasAnim model, NULL = "gltf"
    float anim_s;        // animS (hasAnim)
    float env;           // env map intensity (env || sameEnv), 0 = none
    // ---- v7.1.4 player-model and material-effect fields (WP-F: players.js generateMeshes /
    // generateMeshData, playerMesh.js, pets.js, render.js getMat)
    unsigned flags2;     // VS2_*
    int shader_num;      // shaderNum (VS2_SHADER): ShaderToy material 0-7
    int video;           // video id (VS2_VIDEO): videos/video_<n>
    int particle;        // particle effect id (VS2_PARTICLE): players.itemEffects
    int spray_snd;       // spraySnd (VS2_SPRAY_SND)
    float leg_len;       // shoes legLen: the leg's pants share (genLeg), 0 = 0.75
    float sit_off_x, sy_rot, sz_rot; // wrist: sitOffX, syRot, szRot (sxRot above)
    float glow_int;      // glowInt: emissiveIntensity, 0 = 1
    float min_glo;       // minGlo: the pulse's floor
    float rgb_s, rgb_t;  // rgb hue cycle speed (rgbS || 0.3) and rgbT
    float r_anim_t;      // rAnimT 1: bob (sin * 0.55) instead of a spin
    float r_anim_m;      // rAnimM
    float mov_d, mov_e, rot_t; // movD (scroll direction), movE, rotT (texture rotation)
    float spd;           // pets: follow speed (spd || 0.12)
    float left_hold[3], right_hold[3]; // weapon skins: left/rightHoldX/Y/Z (genArm), 0 = the weapon's
    // ---- v10.0.2
    const char *id_name; // a non-numeric id (paints: "red"), NULL = id
    const char *desc;    // desc (paints, consumables)
    const char *paint_color; // paints: paintColor "#rrggbb", NULL none
    int title;           // the title id the item grants (0 none)
    const char *set;     // item set ("vcore"), NULL none
    int set_min;         // setMin: items of the set to own for its bonus
} ViewerSkin;

enum {
    VS2_NO_BODY = 1 << 0,        // body: no shirt on the body cube (genBody)
    VS2_NO_HAIR = 1 << 1,        // hat / head: no hair band (genHead)
    VS2_RANDOM_LEG = 1 << 2,     // shoes: only one random leg gets the shoe and legLen
    VS2_NO_SLEEVE = 1 << 3,      // dye: no sleeves (genArm)
    VS2_NO_LEFT_SLEEVE = 1 << 4, // wrist: no left sleeve
    VS2_NO_RIGHT_HAND = 1 << 5,  // weapon skin: no right hand
    VS2_NO_CHARM = 1 << 6,       // weapon skin: no charm
    VS2_PROJECTION = 1 << 7,     // screen-space UVs
    VS2_USE_SKIN = 1 << 8,       // dye: every part takes the skin colour
    VS2_SHADER = 1 << 9,         // shader_num is set
    VS2_VIDEO = 1 << 10,         // video is set
    VS2_PARTICLE = 1 << 11,      // particle is set
    VS2_SND = 1 << 12,           // weapon: snd (own shot sound)
    VS2_KILL_SND = 1 << 13,
    VS2_STEP_SND = 1 << 14,      // shoes: step sound "shoe_<id>"
    VS2_JUMP_SND = 1 << 15,      // shoes: jump sound "shoe_<id>"
    VS2_RELOAD_SND = 1 << 16,
    VS2_SPRAY_SND = 1 << 17,
    VS2_EXPLODE = 1 << 18,       // kill VFX
    VS2_SAME_ENV = 1 << 19,      // env map from the texture itself
    VS2_FORCE_ATTACH = 1 << 20,  // weapon skin: forceAttach, the Reflex Sight (attachIndex 0) whatever the pick
    VS2_TRADABLE = 1 << 21,      // v10: the server's tradable bit (market / trades)
    VS2_LIMITED_SPIN = 1 << 22,  // v10 limitedSpin: a noSpin item a wheel may still draw (getSpnItems)
    VS2_PREMIUM_SPIN = 1 << 23,  // v10 premiumSpin: Premium wheel exclusive
    VS2_PAINTABLE = 1 << 24,     // v10 paintTargets: a paint consumable applies to it
    VS2_NO_COMP = 1 << 25,       // noComp (noLegs / noBody / noHead): not in competitive
};

// A dye's colour overrides (players.js generateMeshes): per part up to 4 colours, one picked at
// random per model (UTILS.getRandom); count 0 = keep the class / skin colour.
enum { VIEWER_DYE_SKIN, VIEWER_DYE_SHIRT, VIEWER_DYE_SLEEVE, VIEWER_DYE_PANTS, VIEWER_DYE_WAIST, VIEWER_DYE_SHOE, VIEWER_DYE_HAIR, VIEWER_DYE_PARTS };
typedef struct { int count; int cols[4]; } ViewerDyeColor;
typedef struct {
    int index;           // store index
    ViewerDyeColor parts[VIEWER_DYE_PARTS];
} ViewerDye;

// A weapon's charm mount (weapons.js charm*): mount model at `mount` (scale mount_scl), the charm
// at `offset` + 0.17 y inside it (scale scl)
typedef struct { int set; float mount[3], mount_scl, offset[3], scl; } ViewerCharmMount;
#define VIEWER_CHARM_WEAPONS 32

typedef struct { const char *name; const char *color; } ViewerRarity;
typedef struct { int set; float x_off, y_off, z_rota, scl, x_rot; } ViewerPreview;
typedef struct { int weapon, mid; ViewerPreview preview; } ViewerPreviewRemodel; // store.previews["<weapon>_<mid>"]
typedef struct { int id; int col; const char *name; } ViewerSkinColor;

typedef enum { VIEWER_LIST_TYPE, VIEWER_LIST_MELEE, VIEWER_LIST_WEAPON } ViewerListKind;
// A dropdown's options in V8 object-key order: count pairs (label skin index, value); -1/-1 = Default.
typedef struct {
    ViewerListKind kind;
    int key;     // type for VIEWER_LIST_TYPE, 1-based weapon for VIEWER_LIST_WEAPON
    int season;  // 0 = All
    int count;
    const short *pairs;
} ViewerOptionList;

#define VIEWER_PREVIEW_SLOTS 32

extern const ViewerSkin g_viewer_skins[];
extern const int g_viewer_skin_count;
extern const ViewerRarity g_viewer_rarities[];
extern const int g_viewer_rarity_count;
extern const char *const g_viewer_types[];
extern const int g_viewer_type_count;
extern const ViewerPreview g_viewer_previews[VIEWER_PREVIEW_SLOTS];
extern const ViewerPreviewRemodel g_viewer_preview_remodels[];
extern const int g_viewer_preview_remodel_count;
extern const ViewerSkinColor g_viewer_skin_colors[];
extern const int g_viewer_skin_color_count;
extern const int g_viewer_current_season;
extern const ViewerDye g_viewer_dyes[];
extern const int g_viewer_dye_count;
extern const ViewerCharmMount g_viewer_charm_mounts[VIEWER_CHARM_WEAPONS];
extern const ViewerOptionList g_viewer_lists[];
extern const int g_viewer_list_count;

// game.store.skins[index], NULL when out of range
const ViewerSkin *viewer_skin(int index);
// The dye entry of a type-5 store index, NULL for others
const ViewerDye *viewer_dye(int index);
// The viewer's dev config editors (viewer.js f453 / f454 mutate store.skins[i]): a writable copy
// of the entry that viewer_skin() / viewer_dye() return from then on, until
// viewer_skin_edits_clear(). NULL when out of range / not a dye / out of edit slots.
ViewerSkin *viewer_skin_edit(int index);
ViewerDye *viewer_dye_edit(int index);
void viewer_skin_edits_clear(void);
// weapons.js charm mount of a weapon id, NULL when the weapon takes no charm
const ViewerCharmMount *viewer_charm_mount(int weapon_id);
// The option list of a dropdown, NULL when there is none (e.g. a weapon without skins).
const ViewerOptionList *viewer_option_list(ViewerListKind kind, int key, int season);
// "Name [Rarity]" (or "Default") of option i into buf.
void viewer_option_label(const ViewerOptionList *list, int i, char *buf, size_t n);
int viewer_option_value(const ViewerOptionList *list, int i);
// store.getPreview(skin): store.previews["<weapon>_<mid>"] for a remodel, else store.previews[weapon]
// (or an all-zero entry)
const ViewerPreview *viewer_preview(const ViewerSkin *skin);
// UTILS.getPreview(skin, store) as an asset-relative path ("textures/previews/....png",
// "textures/sprays/<id>.png"); the page URL is html_dom_asset_url() of it.
void viewer_skin_preview_path(const ViewerSkin *skin, char *path, size_t n);
// UTILS.getPreviewExport(skin, store): the file name the Render buttons download.
void viewer_preview_export_name(const ViewerSkin *skin, char *buf, size_t n);
// JS truthiness of skin.tex (a numeric 0 is falsy)
int viewer_skin_tex_truthy(const ViewerSkin *skin);
// A store item's model src and texSrc as RENDER.loadMesh gets them (viewer_scene.c: the showcase,
// cosmetic pages and the spin wheel's item, spin_wheel.c); texture "" = the model's own.
void viewer_item_keys(const ViewerSkin *n, char *model, size_t model_n, char *texture, size_t texture_n);
