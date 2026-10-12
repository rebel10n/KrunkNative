// KrunkNative map editor: internal state shared by client/src/editor/*.c.
// Port of Krunker v7.1.4 src/editor.js (Editor), libs/ObjectInstance.js,
// libs/TransformControls.js and ui/editor.js (GUI) to C over html_dom.h (the shell, WP2 of
// doc/v7/editor.md, is v7; panels not yet ported keep their v3.1.1 shape).
// Module map:
//   editor.c          AppPage hooks, frame tick, free-fly camera, keyboard/mouse, toolbar buttons
//   editor_objects.c  ObjectInstance: serialized JSON per object, property get/set, meshes,
//                     add/remove/duplicate, copy/paste, groups, history (undo/redo)
//   editor_io.c       getMapExport / importMap / save, storage, files, clipboard, test map
//   editor_gui.c      GUI._build: blueprint controls -> Krunker markup, GUI.action dispatch,
//                     folders, sliders, numbers, the colour picker (Pickr "nano")
//   editor_panels.c   toolbar, right panel switch, blank / Scene Graph panels, windows 1-15
//                     (Controls, Settings, Quick Add, Welcome, Templates, ...); the panel
//                     control kit (editor_panel_kit.h)
//   editor_inspector.c the right object panel (the inspector)
//   editor_env.c      the right environment / terrain / zone panels
//   editor_gameconfig.c the left "Essential" map panel (map, game, server, weapon config)
//   editor_render.c   editor GL: bounding boxes, helper volumes, grid, death barrier,
//                     sun marker, light helpers, wireframe, transform gizmo; ray picking;
//                     gizmo drag math (modelScale / fakeScale / one-sided scaling)
//   editor_overlay.c  EditorOverlay: the 2D layer over the view (FPS counter)
//   editor_backup.c   timed backups (initAutoBackup / LocalDB) and window 15's list
//   editor_prompt.c   prompt() / confirm() stand-ins and Krunker's genericPopup / clearPops
//   editor_data.c     generated Krunker data tables (tools/editor/gen_editor_data.py)
//   editor_project.c  open maps (EditorProject): tabs, new/open/close/save, switching (KrunkNative)
//   editor_view.c     offscreen project renders, detached 3D view windows (KrunkNative)
//   editor_bus_link.c the editor bus: other editor processes and MCP tools (KrunkNative)
//
// State split: EditorProject holds one map (objects, history, selection, camera, file, dirty);
// Editor (g_ed) holds the page, input, settings, clipboard and the list of projects. Every
// function that edits "the map" works on g_ed.proj (see editor_project.c for switching).
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
#define ED_PREFAB_GATE 24
#define ED_PREFAB_TRIGGER 29
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
    unsigned char toggle_hidden:1; // mesh hidden by a Settings > Object bounding box toggle (editor_render.c)
    cJSON *history;        // last serialized state seen by the history poll (initHistory)
    cJSON *scratch;        // properties the object panel edits that ObjectInstance never serializes (aiName, canWallJump, AI behaviour, ...)
} EditorObject;

typedef struct {
    int action;            // 'a' add, 'r' remove, 'u' update, 'm' map config (editor_project.c batches)
    uint32_t uuid;
    cJSON *state;          // add/remove: the object; update: the state before; 'm': {map,game,server} before
    uint32_t batch;        // entries with the same nonzero batch undo/redo as one step (bus edits)
    char *label;           // the batch label (on the newest entry of a batch), NULL = derived
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
    float start_msize;     // modelScale: mSize at the start of the drag
    vec3 plane_normal;
} EditorGizmo;

// Terrain / clutter / zone meshes of one project (editor_terrain.c)
typedef struct {
    Geometry *geometry;
    unsigned int vbo, cbo;
    Mesh *mesh;
} EdTerrPart;

typedef struct {
    uint32_t uuid;
    EditorObject *object;
} EdGraveyard;

// One open map ("project"). Everything the v3.1.1 Editor kept per map lives here; the page,
// input and UI state stays in Editor (g_ed). g_ed.proj is the project shown in the editor;
// code that works on "the map" reads g_ed.proj, and editor_project.c switches it (fully
// with ed_project_activate, or for one bus/background operation with ed_project_enter /
// ed_project_leave, which keeps the page UI on the shown project).
typedef struct EditorProject {
    int id;                // local id, 1.. (bus id "<instance>.<id>")
    char path[1024];       // the map file it was opened from / saved to ("" = untitled)
    char name[128];        // tab title: file basename, else the map name
    int dirty;             // unsaved changes since open/save
    unsigned revision;     // bumps on every change (bus "revision")
    unsigned saved_revision;

    Scene *scene;          // object meshes, helpers and terrain of this map
    Camera camera;
    float yaw, pitch;      // PointerLockControls: yawObject.rotation.y, pitchObject.rotation.x

    // objects (objInsts) and selection (transformControl.object)
    EditorObject **objects;
    size_t count, cap;
    uint32_t next_uuid;
    EditorObject *selected;
    uint32_t *pending_group; size_t pending_count, pending_cap; // group selection
    EditorGroup *groups; size_t group_count;
    int obj_count[3];      // objCount: [non-collidable, collidable, spawns]

    // map-level state (editor_model.h EdMapState: mapConfig / serverConfig / custom GUI / scripts)
    cJSON *map_config;     // mapConfig: every v7 default + imported values
    cJSON *server_config;  // {modes:{}, classes:{}, settings:{}, weps:{}, cls:{}, prks:{}} (index-keyed)
    cJSON *extra_map;      // imported top-level keys that are not mapConfig defaults (kept for export)
    cJSON *gui;            // custom GUI elements (EditorOverlay.customGUIs)
    cJSON *scripts;        // {"client": code, "server": code} (KrunkScript, decoded)

    // history
    EditorHistoryEntry *undos; size_t undo_count, undo_cap;
    EditorHistoryEntry *redos; size_t redo_count, redo_cap;
    double history_next;
    EdGraveyard *graveyard; size_t graveyard_count; // removed objects that undo can revive
    uint32_t batch;        // open history batch (ed_history_batch_begin), 0 = none
    uint32_t next_batch;
    const char *batch_label;

    EdTerrPart terrain, clutter, zone, ocean;
    unsigned bus_revision; // last revision / selection announced on the editor bus
    uint32_t bus_selected;
} EditorProject;

typedef struct {
    Client *client;
    int active;

    // projects (editor_project.c): the open maps; `shown` is the one on screen, `proj` the one
    // every ed_* function works on (== shown except inside ed_project_enter/leave)
    EditorProject *shown;
    int ui_stale;          // a panel rebuild was asked for while working on another project
    EditorProject *proj;
    EditorProject **projects;
    size_t project_count;
    int next_project_id;
    EditorProject *lighting_owner; // whose map lighting g_render_lighting / the sky hold

    cJSON *copy;           // copyObjects clipboard (array of serialized objects); shared by every project
    cJSON *settings;       // editor settings, stored as "kro_editor"
    cJSON *advanced;       // advancedOptions values (rotation, scaleMapX, ...)

    // input / controls
    int move_forward, move_backward, move_left, move_right, move_up, move_down, move_sprint;
    int looking;           // right mouse held: PointerLockControls enabled
    double look_last_x, look_last_y;
    double mouse_x, mouse_y; // framebuffer px
    int mods;
    int transform_type, transform_space;
    int face_selection, group_selection, snapping, wireframe;
    int hide_light_cones, hide_models; // Settings > Object bounding box toggles (lightConeAbuse / showModels off)
    float gizmo_size;      // transformControl.size: +/- keys, 0.1 steps (1 = default)
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
    int dirty_tabs;        // rebuild the project tab strip
    double storage_next;
    char status[256];
    double script_at;      // KRUNKNATIVE_EDITOR_SCRIPT (editor.c)
    int script_done;
} Editor;

extern Editor g_ed;

// ---- editor_project.c: open maps, tabs, new/open/close/save (header comment there)
enum { ED_PM_NEW, ED_PM_OPEN, ED_PM_SAVE, ED_PM_SAVE_ALL, ED_PM_CLOSE, ED_PM_NEXT, ED_PM_DETACH, ED_PM_NEW_WINDOW, ED_PM_BUS, ED_PM_ACCESS };
int editor_cli_file_count(void);
const char *editor_cli_file(int i);
void editor_cli_add_file(const char *path);
EditorProject *ed_project_create(void);              // empty map (defaults), not shown
EditorProject *ed_project_new(int activate);
EditorProject *ed_project_open_file(const char *path, int activate); // NULL on failure (message shown)
EditorProject *ed_project_open_text(const char *text, const char *path, int activate);
EditorProject *ed_project_find(int id);
EditorProject *ed_project_resolve(const char *ref);  // id / "<inst>.<id>" / path / basename / map name
const char *ed_project_title(const EditorProject *p);
int ed_abs_path(const char *in, char *out, size_t n);  // absolute path of an existing file
const char *ed_project_map_name(const EditorProject *p);
void ed_project_activate(EditorProject *p);
void ed_project_enter(EditorProject *p, EditorProject **prev);
void ed_project_leave(EditorProject *prev);
int ed_project_background(void);                     // g_ed.proj is not the shown project
long ed_project_save(EditorProject *p, const char *path, int adopt, int pretty); // bytes, -1 failure
void ed_project_save_ui(EditorProject *p);
void ed_project_save_all(void);
int ed_project_close(EditorProject *p, int force);   // 0: dirty and not forced
void ed_project_request_close(EditorProject *p);     // asks when dirty
int ed_project_confirm_quit(void);                   // 1 = nothing unsaved
int ed_projects_dirty(void);
void ed_project_open_ui(void);
void ed_project_cycle(int dir);
void ed_projects_fini(void);
void ed_tabs_render(void);
void ed_project_install(HtmlHandlerEntry *table, size_t *count, size_t max);
int ed_project_key(int key, int mods);
void ed_project_menu(int action);
void ed_mark_changed(void);                          // the map of g_ed.proj changed (dirty, revision, autosave, bus)

// ---- editor_view.c: detached 3D views (native: extra GLFW windows sharing the GL context)
void ed_view_open(EditorProject *p);
void ed_view_new_process(EditorProject *p);          // `client_bin --editor <file>` joining the bus
void ed_view_project_closed(EditorProject *p);
void ed_view_frame(float now, float dt);             // every editor frame (renders + presents the views)
void ed_views_fini(void);
int ed_view_count(void);
// Renders project p from `cam` into an RGBA buffer (w*h*4, rows top-down, malloc'd). Used by
// the views and the bus render tool. flags: ED_RENDER_*.
enum { ED_RENDER_NO_HELPERS = 1, ED_RENDER_NO_FOG = 2, ED_RENDER_ORTHO = 4 };
unsigned char *ed_render_project(EditorProject *p, const Camera *cam, float yaw, float pitch, float ortho_half_h, int w, int h, int flags);

// ---- editor_bus_link.c: the editor bus (shared/include/editor_bus.h) in the editor
int editor_bus_parse_arg(int argc, char **argv, int *i); // app_mode_parse_args: 1 = consumed
void ed_bus_start(void);                              // page enter (when asked for on the CLI)
void ed_bus_poll(void);
void ed_bus_stop(void);
int ed_bus_active(void);
void ed_bus_toggle(void);
void ed_bus_notify_projects(void);
void ed_bus_notify_change(EditorProject *p);
void ed_bus_notify_selection(EditorProject *p);
void ed_bus_clipboard_set(const cJSON *objects);       // UI copy: share with the other windows
void ed_bus_manage_access(void);                      // remembered MCP grants: list + revoke
void ed_log_line(const char *line);                   // console ring for the bus "log" tool

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
void editor_test_map(int host, int at_camera);        // testMap(host, atCamera)
void editor_update_wireframe(void);
void editor_update_snapping(void);
void editor_warning(const char *id, int on, const char *text, const char *icon); // doWarning (icon NULL = "warning")
void editor_clear_warnings(void);
void editor_teleport_popup(void);
void editor_teleport(int reset, float x, float y, float z);
void editor_script_popup(void);          // scriptPopup (key 0, Scripting menu): WP5's KrunkScript window
void editor_texture_list(void);          // textureKey (T): viewModelList / viewTextureList (WP3 pickers)
#define ED_WINDOW_COUNT 15                // window.windows[] (showWindow 1..15)
void editor_message(const char *text);   // chatList info line (Krunker uses alert())
double editor_setting_num(const char *key, double fallback);
int editor_setting_bool(const char *key);
void editor_settings_save(void);
const char *editor_default_settings(void);  // Editor.defaultSettings (JSON)
void editor_apply_lighting(void);
void editor_scale_ui(void);              // scaleUI (window resize, UI Scale)        // initScene/regenAll: map lighting -> g_render_lighting, sky

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
void ed_history_add_map(void);                     // 'm': the map config before a bus map_set
uint32_t ed_history_batch_begin(const char *label);  // following entries form one undo step (0 = nested)
void ed_history_batch_end(uint32_t id);
int ed_history_step(int redo);                     // one step (an entry or a whole batch); 0 = none
size_t ed_history_steps(int redo);
cJSON *ed_history_labels(int redo, int limit);     // newest first
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

// ---- editor_io.c (the map JSON itself: editor_model.c)
void ed_map_defaults(void);
void ed_map_set_all_classes(const char *var, const cJSON *value); // cls[i][var] on every class (the v3 "game" values)
cJSON *ed_map_export(int full);                    // getMapExport(e): map JSON (owned by caller)
cJSON *ed_map_export_flags(int flags);             // ED_EXPORT_* (editor_model.h)
char *ed_map_export_string(void);
int ed_map_import(const char *text);               // importMap; 0 on failure (message shown)
void ed_map_clear(void);
cJSON *ed_save_doc(void);                          // Editor.save(true): {map, groups, selected, cam, history}
void ed_map_store(void);                           // autosave to storage "knEditorMap"
int ed_map_restore(void);
void ed_file_export(void);
void ed_file_import(void);
void ed_file_save_as(void);
void ed_clipboard_copy(const char *text);
char *ed_clipboard_paste(void);
void ed_quick_start(void);                         // quickStart: loadQuick(0)
const cJSON *ed_templates_get(void);               // assets/editor/templates.json {game, script, defaultMaps}; NULL if missing
int ed_load_quick(int index, int script, int default_map); // window.loadQuick: import a template / default map

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
    EG_SWITCH, EG_SLIDER, EG_SELECT, EG_BUTTON, EG_VECTOR3, EG_KEY, EG_TOOLBAR_OPTION, EG_SELECT_CLICK,
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
void ed_gui_folder_begin(EdBuild *b, const char *name, int small, int closed_default, int sub, const char *state_key); // small: 0 standard, 1 small, 2 smaller
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
EdCtl *ed_gui_select_click(EdBuild *b, const char *name, const EdOption *options, int count, int sub, const char *icon, EdActionFn on_click);
EdCtl *ed_gui_key(EdBuild *b, const char *name, int editable, const char *const *extra, int extra_count, int sub);
// value binding: call right after creating the control, before ed_gui_end
void ed_ctl_bind_json(EdCtl *c, cJSON *obj, const char *key);
void ed_ctl_bind_obj(EdCtl *c, const char *prop);
// the controls write their current value into the markup at creation; these build it:
void ed_gui_emit_value(EdBuild *b, EdCtl *c);

void ed_gui_install(HtmlHandlerEntry *table, size_t *count, size_t max); // GUI.* handler names
int ed_gui_set_by_label(HtmlElem root, const char *label, int nth, const char *value); // a user edit of the nth control labelled so
void ed_gui_tick(void);
void ed_gui_color_hide(void);
int ed_gui_key_capture(int key, int action);      // GUI.update.key.keydown

// ---- editor_panels.c
void ed_toolbar_show(void);
void ed_toolbar_account_check(void);             // per frame: rebuild on login / account change
void ed_panel_left_show(void);
void ed_panel_right_blank(void);
void ed_panel_right_object(void);
void ed_inspector_install(HtmlHandlerEntry *table, size_t *count, size_t max); // editor_inspector.c: KE.selectTex / .. / searchItems
void ed_panel_right_environment(void);
void ed_panel_right_object_list(void);             // Object > Scene Graph (panel.right.objectList)
void ed_import_map_popup(void);                    // importMapPopup (File > Import Project)
void ed_panel_right_show(const char *name);        // "environment" "terrain" "zone" "customize" "tools" "grouping"
void ed_panel_refresh_all(void);
const char *ed_panel_right_active(void);
void ed_window_show(int index);                    // windows[n].gen() into #menuWindow
void ed_window_tick(float dt);                     // Editor.render: the Welcome window's counters (vA499)
void ed_panels_install(HtmlHandlerEntry *table, size_t *count, size_t max);

// ---- editor_render.c
void ed_render_init(void);
void ed_render_fini(void);
void ed_render_grid(void);                         // before scene_render (GridHelper)
void ed_render_overlays(void);                     // after scene_render, inside the view viewport
void ed_render_apply_wireframe(EditorProject *p, int on); // ObjectInstance.forceWire on p's object materials
int  ed_pick(double fb_x, double fb_y, int skip_invisible, EditorObject **out, vec3 *hit, int *face);
void ed_view_ray(double fb_x, double fb_y, vec3 *origin, vec3 *dir);
int  ed_gizmo_hit(double fb_x, double fb_y);       // axis mask under the cursor, 0 = none
void ed_gizmo_begin(double fb_x, double fb_y, int axis);
void ed_gizmo_drag(double fb_x, double fb_y);
void ed_gizmo_end(void);
void ed_obj_world_box(const EditorObject *o, float m[16]); // bounding box matrix (unit box -0.5..0.5)
void ed_euler_matrix(vec3 r, float m[9]);          // three.js Euler XYZ rotation matrix (row major)

// ---- editor_overlay.c: EditorOverlay (#editor-overlay): FPS counter (+ custom GUI preview, WP4)
struct Canvas2D;
struct Canvas2D *ed_overlay_canvas(void);          // NULL until the first overlay frame
void ed_overlay_render(double now_s);              // inside the 3D view viewport, after the overlays
void ed_overlay_fini(void);

// ---- small helpers (editor_objects.c)
double ed_round(double v, int decimals);
const cJSON *ed_json_path(const cJSON *root, const char *key);
void ed_json_set_num(cJSON *obj, const char *key, double v);
void ed_json_set_str(cJSON *obj, const char *key, const char *v);
void ed_json_set_bool(cJSON *obj, const char *key, int v);
double ed_json_num(const cJSON *obj, const char *key, double fallback);
const char *ed_json_str(const cJSON *obj, const char *key, const char *fallback);
int ed_json_truthy(const cJSON *v);

// ---- editor_backup.c: timed backups (initAutoBackup / doBackup / LocalDB, window 15)
void ed_backup_tick(double now);                   // every frame: a backup every backupTimer minutes (autoBackups)
void ed_backup_reset_timer(double now);            // initAutoBackup: restart the interval
void ed_backup_do(void);                           // doBackup: KE.localDB.insert(KE.save(true))
cJSON *ed_backup_list(void);                       // [{timestamp, name}], oldest first (owned)
int ed_backup_load(double timestamp);              // importMapBackup; 0 = missing
int ed_backup_clear(void);                         // LocalDB.clear; 1 = ok
void ed_backup_date(double timestamp, char *out, size_t n); // new Date(t).toLocaleString() (en-US)

// ---- editor_prompt.c: window.prompt() / confirm() stand-ins (in-page #knPrompt dialog)
typedef void (*EdPromptFn)(void *user, const char *value); // value NULL = cancelled
void ed_prompt(const char *message, const char *def, EdPromptFn cb, void *user);
void ed_confirm(const char *message, EdPromptFn cb, void *user); // value "1" = OK
// a dialog with several buttons: cb(user, index), -1 = dismissed (Escape). Queued behind an
// open dialog instead of replacing it.
typedef void (*EdChoiceFn)(void *user, int choice);
void ed_choice(const char *title, const char *message, const char *const *buttons, int count, EdChoiceFn cb, void *user);
void ed_prompt_cancel(void);
void ed_prompt_reset(void);                        // page leave: drop open / queued dialogs
int ed_prompt_open(void);
void ed_prompt_install(HtmlHandlerEntry *table, size_t *count, size_t max);
// window.genericPopup(html, class) / clearPops (#popupHolder > #genericPop of editor.html)
HtmlElem ed_generic_popup(const char *html, const char *cls);
void ed_clear_pops(void);
int ed_popup_is(const char *cls);                   // the generic popup is open with that class

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

// ---- editor_terrain.c: Editor.regenTerrain / regenZone (meshes in g_ed.scene); editor_env.c the panels
int ed_terrain_panel_show(const char *name);            // editor_env.c: "terrain" "zone"; 0 = not one of them
void ed_terrain_regen(void);
void ed_zone_regen(void);                                // + ed_ocean_regen (every caller is a regenAll)
void ed_ocean_regen(void);                               // Editor.regenOcean: the ocean preview plane
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
void ed_group_edit(const char *key, const cJSON *value); // editGroup(key, value): every member of the selected group
void ed_group_reflect(void);                            // advancedOptions.groupReflect: reflectObjects(true)

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
