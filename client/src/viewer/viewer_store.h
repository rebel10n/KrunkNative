#pragma once
// Krunker v3.1.1 store data the viewer page reads (src/data/store.js, src/config.js), generated
// into viewer_store_table.c by tools/ui-import/gen_viewer_store.js. Client-only (the shared code
// and cosmetics_table.c keep their own subsets).
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
    VS_NOT_IN_WHEEL = 1 << 16, // limited / illicit / limT: never in a prize wheel's pool (getSpnItems)
};

typedef struct {
    const char *name;
    int type;            // 0 weapon skin, 1 hat, 2 body (back), 3 melee, 4 spray, 5 dye, 6 waist, 7 face
    int weapon;          // weapon skins: 1-based weapon index (game.weapons[weapon - 1]), else 0
    int id;              // model / skin id, -1 when absent
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
} ViewerSkin;

typedef struct { const char *name; const char *color; } ViewerRarity;
typedef struct { int set; float x_off, y_off, z_rota, scl, x_rot; } ViewerPreview;
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
extern const ViewerSkinColor g_viewer_skin_colors[];
extern const int g_viewer_skin_color_count;
extern const int g_viewer_current_season;
extern const ViewerOptionList g_viewer_lists[];
extern const int g_viewer_list_count;

// game.store.skins[index], NULL when out of range
const ViewerSkin *viewer_skin(int index);
// The option list of a dropdown, NULL when there is none (e.g. a weapon without skins).
const ViewerOptionList *viewer_option_list(ViewerListKind kind, int key, int season);
// "Name [Rarity]" (or "Default") of option i into buf.
void viewer_option_label(const ViewerOptionList *list, int i, char *buf, size_t n);
int viewer_option_value(const ViewerOptionList *list, int i);
// store.previews[weapon] (or an all-zero entry)
const ViewerPreview *viewer_preview(int weapon);
// UTILS.getPreview(skin, store) as an asset-relative path ("textures/previews/....png",
// "textures/sprays/<id>.png"); the page URL is html_dom_asset_url() of it.
void viewer_skin_preview_path(const ViewerSkin *skin, char *path, size_t n);
// UTILS.getPreviewExport(skin, store): the file name the Render buttons download.
void viewer_preview_export_name(const ViewerSkin *skin, char *buf, size_t n);
// JS truthiness of skin.tex (a numeric 0 is falsy)
int viewer_skin_tex_truthy(const ViewerSkin *skin);
