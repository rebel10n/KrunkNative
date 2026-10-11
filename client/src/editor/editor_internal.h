// KrunkNative map editor: internal state shared by client/src/editor/*.c.
// Port of Krunker v3.1.1 public/js/editor.js (Editor), libs/ObjectInstance.js,
// libs/TransformControls.js and ui/editor.js (GUI) to C over html_dom.h.
// Module map:
//   editor.c          AppPage hooks, frame tick, free-fly camera, keyboard/mouse, toolbar buttons
//   editor_objects.c  ObjectInstance: serialized JSON per object, property get/set, meshes,
//                     add/remove/duplicate, copy/paste, groups, history (undo/redo)
//   editor_io.c       getMapExport / importMap / save, storage, files, clipboard, test map
//   editor_gui.c      GUI._build: blueprint controls -> Krunker markup, GUI.action dispatch,
//                     folders, sliders, numbers, the colour picker (Pickr "nano")
//   editor_panels.c   toolbar, left "Essential" map panel, right object / environment / ...
//                     panels, windows (Controls, Settings, Quick Add)
//   editor_render.c   editor GL: bounding boxes, helper volumes, grid, death barrier,
//                     sun marker, transform gizmo; ray picking; gizmo drag math
//   editor_data.c     generated Krunker data tables (tools/editor/gen_editor_data.py)
#pragma once
#include <gl.h> // glad natively, GLES3 on the web (before GLFW)
#include <client.h>
#include <cJSON.h>
#include <html_dom.h>
#include <stddef.h>
#include <stdint.h>
#include "editor_data.h"

// Krunker prefab ids the editor refers to by number (data/prefabs.js order)
#define ED_PREFAB_CUBE 0
#define ED_PREFAB_SPAWN_POINT 5
#define ED_PREFAB_CAMERA_POSITION 6
#define ED_PREFAB_PLACEHOLDER 35
#define ED_PREFAB_CUSTOM_ASSET 44

typedef struct EditorObject {
    uint32_t uuid;
    int prefab;
    cJSON *data;           // ObjectInstance.serialize() form: {"p","s","i"?,...}; colours inline (c/e), never ci/ei
    // rendering (editor_objects.c / editor_render.c)
    cJSON *render_doc;     // {"objects":[copy of data],"spawns":[]} kept alive for render_map
    Map *render_map;       // one-object map built by map_init(): owns the mesh
    Mesh *mesh;            // render_map->objects[0]->mesh (NULL for tools and hidden prefabs)
    Mesh *helper;          // editor-only model (spawn arrow), owned here
    unsigned int helper_geometry_owned:1;
    unsigned char highlighted:1; // group-selection pending
    cJSON *history;        // last serialized state seen by the history poll (initHistory)
    cJSON *scratch;        // properties the object panel edits that ObjectInstance never serializes (aiName, canWallJump, AI behaviour, ...)
} EditorObject;

typedef struct {
    int action;            // 'a' add, 'r' remove, 'u' update
    uint32_t uuid;
    cJSON *state;          // add/remove: the object; update: the state before
} EditorHistoryEntry;

typedef struct {
    uint32_t owner;        // placeholder object uuid
    uint32_t *objects;     // member uuids
    size_t count;
    vec3 pos, scale;       // owner transform when the group was made / last synced
    float rot_y;
} EditorGroup;

enum { ED_TRANSLATE, ED_ROTATE, ED_SCALE };
enum { ED_SPACE_WORLD, ED_SPACE_LOCAL };

typedef struct {
    int active;            // dragging a gizmo handle
    int axis;              // bit mask X=1 Y=2 Z=4 (translate/scale planes use two bits); 8 = view plane/uniform
    int hover;             // axis mask under the cursor
    vec3 start_pos, start_size, start_rot;
    vec3 start_hit;        // world point where the drag started (translate/scale)
    float start_angle;     // rotate
    vec3 plane_normal;
} EditorGizmo;

typedef struct {
    Client *client;
    int active;
    Scene *scene;
    Camera camera;
    float yaw, pitch;      // PointerLockControls: yawObject.rotation.y, pitchObject.rotation.x

    // objects (objInsts) and selection (transformControl.object)
    EditorObject **objects;
    size_t count, cap;
    uint32_t next_uuid;
    EditorObject *selected;
    uint32_t *pending_group; size_t pending_count, pending_cap; // group selection
    EditorGroup *groups; size_t group_count;
    cJSON *copy;           // copyObjects clipboard (array of serialized objects, relative)
    int obj_count[3];      // objCount: [non-collidable, collidable, spawns]

    // map-level state (mapConfig / gameConfig / serverConfig / settings / advancedOptions)
    cJSON *map_config;
    cJSON *game_config;
    cJSON *server_config;  // {modes:{}, classes:{}, settings:{}, weps:{}}
    cJSON *settings;       // editor settings, stored as "kro_editor"
    cJSON *advanced;       // advancedOptions values (rotation, scaleMapX, ...)
    cJSON *extra_map;      // imported map fields the editor does not model (kept for export)

    // history
    EditorHistoryEntry *undos; size_t undo_count, undo_cap;
    EditorHistoryEntry *redos; size_t redo_count, redo_cap;
    double history_next;

    // input / controls
    int move_forward, move_backward, move_left, move_right, move_up, move_down, move_sprint;
    int looking;           // right mouse held: PointerLockControls enabled
    double look_last_x, look_last_y;
    double mouse_x, mouse_y; // framebuffer px
    int mods;
    int transform_type, transform_space;
    int face_selection, group_selection, snapping;
    EditorGizmo gizmo;
    double last_click_time; double last_click_x, last_click_y;

    // layout (framebuffer px) of #container and CSS -> fb scale
    int view_x, view_y, view_w, view_h;
    float css_to_fb;       // fb px per CSS px (viewport scale)
    float elem_scale;      // scaleUI(): uiBase transform scale
    int fb_w, fb_h;
    int panel_hidden[2];   // Resizable.hide/restore (left, right)
    float panel_width[2];

    // page elements
    HtmlElem el_container, el_center, el_left_list, el_right_list, el_menu_window, el_window_holder;
    int window_index;      // open window (showWindow) or -1
    int menu_open;

    // misc
    double now;
    int dirty_panel;       // rebuild the right panel next tick (updateObjConfigGUI)
    int dirty_left;
    int dirty_counts;
    int dirty_storage;     // autosave the map to storage
    double storage_next;
    char status[256];
    double script_at;      // KRUNKNATIVE_EDITOR_SCRIPT (editor.c)
    int script_done;
} Editor;

extern Editor g_ed;

// ---- editor.c
void editor_set_transform_type(int type);
void editor_set_transform_space(int space);
void editor_update_face_selection(void);
void editor_update_group_selection(void);
void editor_update_object_count(void);
void editor_toggle_menu(int open);
void editor_show_window(int index);
void editor_close_window(void);
void editor_frame_object(EditorObject *o);
void editor_camera_dir(vec3 *forward, vec3 *right, vec3 *up);
vec3 editor_near_position(void);
void editor_test_map(int host);
void editor_message(const char *text);   // chatList info line (Krunker uses alert())
double editor_setting_num(const char *key, double fallback);
int editor_setting_bool(const char *key);
void editor_settings_save(void);
const char *editor_default_settings(void);  // Editor.defaultSettings (JSON)
void editor_apply_lighting(void);        // initScene/regenAll: map lighting -> g_render_lighting, sky

// ---- editor_objects.c
EditorObject *ed_obj_new(const cJSON *serialized, int keep_uuid);  // deserialize (copies)
void ed_obj_free(EditorObject *o);
void ed_obj_rebuild(EditorObject *o);              // regen: mesh from data
void ed_obj_add(EditorObject *o, int no_select, int history);
void ed_obj_remove(EditorObject *o, int keep_selection, int history);
EditorObject *ed_obj_find(uint32_t uuid);
void ed_obj_select(EditorObject *o);               // attachTransform (NULL = hideTransform)
void ed_obj_duplicate(void);
void ed_obj_delete_selected(void);
void ed_obj_copy(int cut, int as_group);
void ed_obj_paste(void);
void ed_obj_create(int prefab);                    // createObjects[i]()
void ed_obj_create_placeholder(void);
void ed_obj_toggle_prop(const char *prop);         // toggleProp (shift+C/V)
void ed_obj_toggle_prop_group(const char *prop);   // the members of a selected group, else toggleProp
void ed_obj_flip_xz(void);
cJSON *ed_obj_serialize(const EditorObject *o);    // copy of data, normalised
int ed_obj_count_class(const EditorObject *o);     // cntCheck
void ed_objects_clear(void);

vec3 ed_obj_vec(const EditorObject *o, const char *key, vec3 fallback); // p / s / r
void ed_obj_set_vec(EditorObject *o, const char *key, vec3 v);
// ObjectInstance properties by editor name ("collidable", "texture", "color", ...)
cJSON *ed_prop_get(const EditorObject *o, const char *prop);        // new cJSON value
void ed_prop_set(EditorObject *o, const char *prop, const cJSON *value);
int ed_prop_needs_rebuild(const char *prop);

void ed_history_add(int action, EditorObject *o, cJSON *state);
void ed_history_execute(int redo);
void ed_history_poll(void);
void ed_history_clear(void);

void ed_group_create(void);                        // copyObjects(false, true): objects under the selected placeholder -> group
void ed_group_from_pending(void);                  // updateGroupSelection: picked objects -> group
// the editor.js methods with an explicit target (NULL = the selection; the Quick GUI passes its object)
void ed_duplicate_object(EditorObject *e);
void ed_duplicate_group(EditorObject *e);
void ed_remove_group(EditorObject *e);
void ed_copy_objects(int cut, int group, EditorObject *sel);
cJSON *ed_copy_group(EditorObject *e);             // copyGroup: sets g_ed.copy (borrowed)
void ed_paste_objects(EditorObject *e);
int ed_is_grouped(uint32_t uuid);
void ed_fix_hitbox(EditorObject *e);               // editor_quickgui.c: fixHitbox (Shift+F)
void ed_group_stop(int all);
void ed_groups_sync(void);                         // updateGroups: members follow their placeholder
EditorGroup *ed_group_of(const EditorObject *owner);

// ---- editor_io.c
void ed_map_defaults(void);
cJSON *ed_map_export(int full);                    // getMapExport(e): map JSON (owned by caller)
char *ed_map_export_string(void);
int ed_map_import(const char *text);               // importMap; 0 on failure (message shown)
void ed_map_clear(void);
void ed_map_store(void);                           // autosave to storage "knEditorMap"
int ed_map_restore(void);
void ed_file_export(void);
void ed_file_import(void);
void ed_file_save_as(void);
void ed_clipboard_copy(const char *text);
char *ed_clipboard_paste(void);
void ed_quick_start(void);

// ---- editor_gui.c
typedef enum {
    EB_NONE, EB_JSON, EB_OBJ, EB_ACTION,
} EdBindKind;

typedef struct EdCtl EdCtl;
typedef void (*EdChangeFn)(EdCtl *ctl, const cJSON *value);
typedef void (*EdActionFn)(EdCtl *ctl);

struct EdCtl {
    int id;
    int kind;              // EG_*
    EdBindKind bind;
    cJSON *json;           // EB_JSON target object
    char key[64];          // EB_JSON key / EB_OBJ property
    int value_is_string;   // select with string values (texture)
    EdChangeFn on_change;
    EdActionFn action;
    int arg;
    char path[160];        // folder: state key; key: extra " or ..." HTML (arrow icons)
};

enum {
    EG_PANEL_BUTTONS, EG_RULE, EG_FOLDER_STANDARD, EG_FOLDER_SMALL, EG_TEXT, EG_NUMBER, EG_COLOR,
    EG_SWITCH, EG_SLIDER, EG_SELECT, EG_BUTTON, EG_VECTOR3, EG_KEY, EG_TOOLBAR_OPTION,
};

typedef struct {
    char *buf;
    size_t len, cap;
} EdBuf;

void edb_init(EdBuf *b);
void edb_free(EdBuf *b);
void edb_add(EdBuf *b, const char *s);
void edb_addf(EdBuf *b, const char *fmt, ...);
void edb_add_escaped(EdBuf *b, const char *s);     // HTML attribute/text escaping

// Panel builder: controls are registered in a per-panel table; markup is Krunker's.
typedef struct {
    int panel;             // 0 left, 1 right, 2 window, 3 toolbar
    EdBuf html;
    int depth_closed;      // inside a closed folder (no effect on markup)
} EdBuild;

typedef struct {
    const char *name;
    double value;
    const char *svalue;    // string-valued options (texture names)
} EdOption;

void ed_gui_begin(EdBuild *b, int panel);
void ed_gui_end(EdBuild *b, HtmlElem target);       // innerHTML
void ed_gui_reset_panel(int panel);
EdCtl *ed_gui_ctl(int panel, int kind);
void ed_gui_panel_buttons(EdBuild *b, int right, const char *label, int blank);
void ed_gui_rule(EdBuild *b);
void ed_gui_folder_begin(EdBuild *b, const char *name, int small, int closed_default, int sub, const char *state_key);
void ed_gui_folder_end(EdBuild *b, int small);
EdCtl *ed_gui_text(EdBuild *b, const char *name, const char *placeholder, int sub);
EdCtl *ed_gui_number(EdBuild *b, const char *name, const char *placeholder, double min, int sub);
EdCtl *ed_gui_color(EdBuild *b, const char *name, int sub);
EdCtl *ed_gui_switch(EdBuild *b, const char *name, int sub);
EdCtl *ed_gui_slider(EdBuild *b, const char *name, double min, double max, double step, int sub);
EdCtl *ed_gui_select(EdBuild *b, const char *name, const EdOption *options, int count, int sub);
EdCtl *ed_gui_button(EdBuild *b, const char *label, EdActionFn fn);
EdCtl *ed_gui_button_line(EdBuild *b, const char *name, const char *label, EdActionFn fn, int sub);
EdCtl *ed_gui_vector3(EdBuild *b, const char *name, double min);
EdCtl *ed_gui_key(EdBuild *b, const char *name, int editable, const char *const *extra, int extra_count, int sub);
// value binding: call right after creating the control, before ed_gui_end
void ed_ctl_bind_json(EdCtl *c, cJSON *obj, const char *key);
void ed_ctl_bind_obj(EdCtl *c, const char *prop);
// the controls write their current value into the markup at creation; these build it:
void ed_gui_emit_value(EdBuild *b, EdCtl *c);

void ed_gui_install(HtmlHandlerEntry *table, size_t *count, size_t max); // GUI.* handler names
void ed_gui_tick(void);
void ed_gui_color_hide(void);
int ed_gui_key_capture(int key, int action);      // GUI.update.key.keydown

// ---- editor_panels.c
void ed_toolbar_show(void);
void ed_panel_left_show(void);
void ed_panel_right_blank(void);
void ed_panel_right_object(void);
void ed_panel_right_environment(void);
void ed_panel_right_show(const char *name);        // "environment" "terrain" "zone" "customize" "tools" "grouping"
void ed_panel_refresh_all(void);
const char *ed_panel_right_active(void);
void ed_window_show(int index);                    // windows[n].gen() into #menuWindow
void ed_panels_install(HtmlHandlerEntry *table, size_t *count, size_t max);

// ---- editor_render.c
void ed_render_init(void);
void ed_render_fini(void);
void ed_render_grid(void);                         // before scene_render (GridHelper)
void ed_render_overlays(void);                     // after scene_render, inside the view viewport
int  ed_pick(double fb_x, double fb_y, int skip_invisible, EditorObject **out, vec3 *hit, int *face);
void ed_view_ray(double fb_x, double fb_y, vec3 *origin, vec3 *dir);
int  ed_gizmo_hit(double fb_x, double fb_y);       // axis mask under the cursor, 0 = none
void ed_gizmo_begin(double fb_x, double fb_y, int axis);
void ed_gizmo_drag(double fb_x, double fb_y);
void ed_gizmo_end(void);
void ed_obj_world_box(const EditorObject *o, float m[16]); // bounding box matrix (unit box -0.5..0.5)
void ed_euler_matrix(vec3 r, float m[9]);          // three.js Euler XYZ rotation matrix (row major)

// ---- small helpers (editor_objects.c)
double ed_round(double v, int decimals);
const cJSON *ed_json_path(const cJSON *root, const char *key);
void ed_json_set_num(cJSON *obj, const char *key, double v);
void ed_json_set_str(cJSON *obj, const char *key, const char *v);
void ed_json_set_bool(cJSON *obj, const char *key, int v);
double ed_json_num(const cJSON *obj, const char *key, double fallback);
const char *ed_json_str(const cJSON *obj, const char *key, const char *fallback);
int ed_json_truthy(const cJSON *v);

// ---- editor_prompt.c: window.prompt() / confirm() stand-ins (in-page #knPrompt dialog)
typedef void (*EdPromptFn)(void *user, const char *value); // value NULL = cancelled
void ed_prompt(const char *message, const char *def, EdPromptFn cb, void *user);
void ed_confirm(const char *message, EdPromptFn cb, void *user); // value "1" = OK
void ed_prompt_cancel(void);
int ed_prompt_open(void);
void ed_prompt_install(HtmlHandlerEntry *table, size_t *count, size_t max);

// ---- editor_net.c: asset / mod / texture browsers (windows 2, 5, 7), publishing (social socket)
// "Select Custom" / "Select Sound" / "Select Mod" buttons; c->arg: 0 selectModel (BOT assetID),
// 1 selectTexture, 2 selectAudio (assetSound), 3 selectDeath, 4 selectIdle, 5 selectAttack,
// 6 selectAgro, 7 selectMod
void ed_net_select(EdCtl *c);
int ed_net_window_gen(int index, EdBuf *out);           // windows 2, 5, 7, 8 into out; 0 = not one of them
void ed_net_view_assets(int type);                      // viewAssets(type): the Assets window for a CUSTOM_ASSET / BOT model
void ed_net_publish(void);                              // toolbar Publish... (publishMap)
void ed_net_frame(void);                                // page_tick after the 3D view is drawn (publish thumbnail)
int ed_net_keydown(const HtmlEvent *ev);                // Enter in a browser search field; 1 = handled
void ed_net_script_publish(const char *arg);             // KRUNKNATIVE_EDITOR_SCRIPT "publish[:category]"
void ed_net_reset(void);                                // page leave: drop cached lists
void ed_net_install(HtmlHandlerEntry *table, size_t *count, size_t max);

// ---- editor_terrain.c: Terrain / Zone panels, Editor.regenTerrain / regenZone (meshes in g_ed.scene)
int ed_terrain_panel_show(const char *name);            // "terrain" "zone"; 0 = not one of them
void ed_terrain_regen(void);
void ed_zone_regen(void);
void ed_terrain_fini(void);                              // page_leave: drop both before the scene

// ---- editor_tools.c: Customize / Tools / Grouping panels, replaceObject, createGroup, loadFile
int ed_panel_right_begin(EdBuild *b, const char *name);  // editor_panels.c: 0 = another panel is locked
int ed_tools_panel_show(const char *name);              // "customize" "tools" "grouping"; 0 = not one of them
void ed_find_center(const cJSON *arr, double out[6]);   // findCenter: [x, minY, z, w, h, d]
void ed_merge_objects(cJSON *arr);                      // mergeObjects
void ed_reflect_array(cJSON *arr, int axis);            // reflect(e, t, false)
void ed_obj_replace(EditorObject *target, const char *json, const char *type, int group); // replaceObject
void ed_create_group(EditorObject *replaced, EditorObject **members, size_t count, double rot_y, int remove);
void ed_json_input(int file);                           // jsonInput(file)
void ed_json_input_for(int file, EditorObject *target); // jsonInput(file, t) (NULL = selection)
void ed_export_group(void);                             // exportGroup (Object > Group > Prefab > Export)

// ---- editor_quickgui.c: right-click Quick GUI, hitbox fixer, object painter
void ed_quickgui_show(EditorObject *o, double fb_x, double fb_y); // updateQuickGUI
void ed_quickgui_hide(void);
int ed_quickgui_hidden(void);
void ed_quickgui_reset(void);                           // page leave
int ed_quickgui_button(int glfw_button);                // the Open On button (settings.quickGUIOpen)
void ed_quickgui_cursor(double fb_x, double fb_y);      // title bar drag
void ed_quickgui_release(void);
int ed_object_painter_click(vec3 hit);                  // shift+click painter; 1 = handled
void ed_quickgui_install(HtmlHandlerEntry *table, size_t *count, size_t max);
typedef void (*EdFileFn)(void *user, const char *path);
void ed_pick_file(const char *accept, EdFileFn fn, void *user); // loadFile: a picked file's path
void ed_download(const char *text, const char *name);   // editor_io.c: download(text, name)
