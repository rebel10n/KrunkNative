#pragma once
// Shared state of the viewer page modules (client/src/viewer/): viewer.c (page, params, the
// dat.gui panel and its callbacks), viewer_scene.c (three.js scene port: player / item meshes,
// lights, orbit camera, drawing), viewer_import.c (Load / drop / zip imports, downloads).
#include <client.h>
#include "viewer_gui.h"
#include "viewer_store.h"

// viewer.js `E`: the panel's bound properties
typedef struct {
    VgValue skin_prim, skin_sec, face, hat, back, waist, melee, skin_col, dye, secondary, class_index, attach, season;
    VgValue lighting;         // "Viewer" / "In-game"
    int show_player, show_back, show_waist, show_face, show_hat, show_arms, show_legs, show_wep;
    int gradient, animate, crouch;
} ViewerState;

typedef struct {
    ViewerState e;
    int swap;            // T: 0 primary, -1 secondary, 1 melee
    int cosmetic;        // C: cosmetic=<store index> (single item render page), -1 = none
    int cosmetic_set;    // UTILS.isNumber(C)
    int showcase;        // P: showcase=<store index>, -1 = none
    int showcase_set;
    int auto_render;     // D: params.has("auto")
    int no_tex;          // B: params.has("noTex")
    int no_bg;           // N: params.has("nobg")
    int dev;             // params.has("dev")
    int hi_quality;      // I = isNumber(C) || params.get("dev")
    int export_size;     // params.get("size") || 256
    int nogui;
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
void vscene_build_cosmetic(int skin_index); // F(C): weapon / melee item on its own
void vscene_build_showcase(int skin_index); // the showcase=<index> page
void vscene_update_lights(void);            // U()
void vscene_camera_player(int show_player); // Show Model onChange camera move
void vscene_camera_weapon(void);            // V()
void vscene_camera_render_pose(int what);   // renderHat/Face/Back/Waist/Dye camera + model pose
void vscene_tick(float dt_ms);
void vscene_render(int fb_w, int fb_h);
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
const Weapon *vscene_player_weapon(void);   // d.weapon
int  vscene_player_is_melee(void);
// the size the scene renders at (cosmetic page: 512 x 512 CSS px at the container's top-left)
void vscene_canvas_rect(float *x, float *y, float *w, float *h);

// override a cached texture / model ("hats/hat_3", "weapons/weapon_2") with new data;
// RENDER.updateTexture / updateMesh. viewer_scene_reset_overrides() = RENDER.reset().
int  vscene_update_texture_rgba(const char *key, const unsigned char *rgba, int w, int h);
int  vscene_update_mesh_obj(const char *key, const char *obj_path);
void vscene_reset_overrides(void);

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
