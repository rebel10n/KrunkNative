#pragma once
// Shared state of the viewer page modules (client/src/viewer/): viewer.c (page, params, the
// dat.gui panel and its callbacks), viewer_scene.c (three.js scene port: player / item meshes,
// lights, orbit camera, drawing), viewer_import.c (Load / drop / zip imports, downloads),
// viewer_config.c (the dev config editors).
#include <client.h>
#include "viewer_gui.h"
#include "viewer_store.h"

// viewer.js `vO201`: the panel's bound properties (v7.1.4: body is store type 2, back type 14)
typedef struct {
    VgValue skin_prim, skin_sec, face, hat, head, body, back, waist, shoe, pet, wrist, melee, skin_col, dye, secondary, class_index, attach, season;
    VgValue lighting;         // "Viewer" / "In-game"
    int show_player, show_body, show_back, show_waist, show_face, show_hat, show_head, show_pet, show_wrist, show_arms, show_legs, show_wep;
    int gradient, animate, crouch;
} ViewerState;

typedef struct {
    ViewerState e;
    int swap;            // T: 0 primary, -1 secondary, 1 melee
    int cosmetic;        // C: cosmetic=<store index> (single item render page), -1 = none
    int cosmetic_set;    // UTILS.isNumber(C)
    int showcase;        // P: showcase=<store index>, -1 = none
    int showcase_set;
    WornSkinPaint paint; // v10.0.3 paint=<rrggbb> / paintShader=<n> (deob-10.0.3.js:123160-123173): the shown copy's paint
    int seed;            // v10.0.4 seed=<n> (deob-10.0.4.js:109446): the shown copy's seed (skin_fx_copy_seed), 0 none
    int auto_render;     // D: params.has("auto")
    int no_tex;          // B: params.has("noTex")
    int no_bg;           // N: params.has("nobg")
    int dev;             // params.has("dev")
    int hi_quality;      // I = isNumber(C) || params.get("dev")
    int export_size;     // params.get("size") || 256
    int nogui;
    int freeze;          // KRUNKNATIVE_VIEWER_FREEZE=1: no clock (render deltas 0, no auto-rotate), for captures
    Client *client;
} Viewer;

extern Viewer g_viewer;

// URL params (viewer.js `params`)
int viewer_param_has(const char *key);
const char *viewer_param_get(const char *key); // NULL when absent

// ---- viewer_scene.c
int  vscene_init(void);
void vscene_fini(void);
void vscene_rebuild_player(void);           // Z()
void vscene_build_cosmetic(int skin_index); // f447(C): one store item on its own (cosmetic page)
void vscene_build_showcase(int skin_index); // the showcase=<index> page
void vscene_update_lights(void);            // U()
void vscene_camera_player(int show_player); // Show Model onChange camera move
void vscene_camera_weapon(void);            // V()
void vscene_camera_render_pose(int what);   // renderHat/Head/Face/Body/Back/Waist/Shoes/Dye/Pet camera + model pose (store type)
int  vscene_cosmetic_shown(int skin_index); // f447: the cosmetic page renders this item
void vscene_tick(float dt_ms);
void vscene_render(int fb_w, int fb_h);
// draw the scene into the framebuffer rectangle (px, bottom-left origin) without clearing its
// colour: the item view's showcase over the page (viewer_embed.h)
void vscene_render_rect(int x, int y, int w, int h, int fb_w, int fb_h);
// Draws one frame offscreen at w x h and returns RGBA rows top-first (malloc'd), optionally over
// the screenshot gradient (createLinearGradient(0, 0, 0, width)); NULL on failure.
unsigned char *vscene_capture(int w, int h, int gradient);
// orbit controls (three.js OrbitControls), CSS px
void vscene_orbit_begin(int button, int mods, float x, float y);
void vscene_orbit_move(float x, float y);
void vscene_orbit_end(void);
int  vscene_orbit_active(void);
void vscene_orbit_wheel(float dy);
void vscene_orbit_key(int dom_key_code);
// the current player's state, for the import targets
int  vscene_player_weapon_slot(void);       // weaponIndex
int  vscene_player_weapon_id(void);         // loadout[weaponIndex], -1 without a player
const Weapon *vscene_player_weapon(void);   // d.weapon
int  vscene_player_is_melee(void);
// the size the scene renders at (cosmetic page: 512 x 512 CSS px at the container's top-left)
void vscene_canvas_rect(float *x, float *y, float *w, float *h);

// override a cached texture / model ("hats/hat_3", "weapons/weapon_2") with new data;
// RENDER.updateTexture / updateMesh. viewer_scene_reset_overrides() = RENDER.reset().
int  vscene_update_texture_rgba(const char *key, const unsigned char *rgba, int w, int h);
int  vscene_update_mesh_obj(const char *key, const char *obj_path);
int  vscene_update_mesh_gltf(const char *key, const char *file_path, const char *ext); // "gltf" / "glb"
void vscene_reset_overrides(void);

// ---- viewer_config.c: [dev] the "<Slot> Config" / "<Weapon> Config" folders (viewer.js f453 / f454)
void vconfig_reset(void);                     // page enter: no targets, no edits
// addFolder(name) + f453(type) / f454: type is the store type, or -1 / -2 / -3 for the primary /
// secondary / melee skin (vA353[-n]); hidden until its dropdown picks an item
void vconfig_folder(VgFolder *parent, int type, const char *name);
void vconfig_remove_folders(VgFolder *parent); // f450 / f451 removeFolder()
void vconfig_snapshot(void);                  // the dropdowns' values now are not changes
void vconfig_changed(void);                   // a dropdown changed: vA353[type] = that skin, f453 / f454

// ---- viewer_import.c
void viewer_import_init(Client *client);
void viewer_import_fini(void);
void viewer_import_tick(void);
void viewer_import_pick(int max_files, int target);  // J(n): file picker, target 0 = weapon skin
void viewer_import_path(const char *path, int target); // one picked / dropped file (Q)
void viewer_download_png(const char *name, const unsigned char *rgba, int w, int h);
// the alert() / prompt() replacements (modal in the page)
void viewer_alert(const char *message);
void viewer_prompt(const char *title, void (*done)(const char *text));
void viewer_dialog_register_handlers(HtmlHandlerEntry *table, int *count);
