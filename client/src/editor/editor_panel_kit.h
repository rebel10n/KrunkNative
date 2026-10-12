// Map editor panel kit: the control helpers shared by the panel files (editor_panels.c,
// editor_inspector.c, editor_env.c, editor_gameconfig.c). Each creates a control with
// editor_gui.c, binds it to an object property (obj_*) or a JSON member (json_*) and writes
// its current value into the markup. Defined in editor_panels.c.
#pragma once
#include "editor_internal.h"

enum { PANEL_LEFT, PANEL_RIGHT, PANEL_WINDOW, PANEL_TOOLBAR };

EdCtl *edp_finish(EdBuild *b, EdCtl *c, EdChangeFn fn);
EdCtl *edp_obj_switch(EdBuild *b, const char *name, const char *prop, int sub, EdChangeFn fn);
EdCtl *edp_obj_slider(EdBuild *b, const char *name, const char *prop, double mn, double mx, double step, int sub, EdChangeFn fn);
EdCtl *edp_obj_select(EdBuild *b, const char *name, const char *prop, const EdOption *opts, int n, int sub, EdChangeFn fn);
EdCtl *edp_obj_color(EdBuild *b, const char *name, const char *prop, int sub);
EdCtl *edp_obj_text(EdBuild *b, const char *name, const char *prop, const char *placeholder, int sub, EdChangeFn fn);
EdCtl *edp_obj_number(EdBuild *b, const char *name, const char *prop, const char *placeholder, int sub);
EdCtl *edp_obj_vector3(EdBuild *b, const char *name, const char *prop, EdChangeFn fn);
EdCtl *edp_json_switch(EdBuild *b, const char *name, cJSON *obj, const char *key, int sub, EdChangeFn fn);
EdCtl *edp_json_slider(EdBuild *b, const char *name, cJSON *obj, const char *key, double mn, double mx, double step, int sub, EdChangeFn fn);
EdCtl *edp_json_text(EdBuild *b, const char *name, cJSON *obj, const char *key, const char *placeholder, int sub, EdChangeFn fn);
EdCtl *edp_json_select(EdBuild *b, const char *name, cJSON *obj, const char *key, const EdOption *opts, int n, int sub, EdChangeFn fn);
EdCtl *edp_json_color(EdBuild *b, const char *name, cJSON *obj, const char *key, int sub, EdChangeFn fn);
EdCtl *edp_json_number(EdBuild *b, const char *name, cJSON *obj, const char *key, const char *placeholder, int sub, EdChangeFn fn);
cJSON *edp_child_obj(cJSON *parent, const char *key);           // parent[key], made an object if it is not
void edp_ensure_default(cJSON *obj, const char *key, const cJSON *def);

// options built from a JSON string list; the arrays live for the panel's lifetime
#define OPT_POOL 4096
extern EdOption g_opt_pool[OPT_POOL];
extern char g_opt_names[OPT_POOL][48];                         // label storage, by g_opt_pool slot
EdOption *edp_opts_alloc(int n);
EdOption *edp_opts_from_names(const cJSON *names, int *count);
const char *edp_const_name(const char *s, char *out, size_t n); // UTILS.formatConstName
void edp_div_begin(EdBuild *b, const char *id, int show);      // _build "div" (closed with "</div>")

// right panel: 0 when another panel is locked; edp_right_begin starts building `name`
int edp_right_can_show(const char *name);
void edp_right_begin(EdBuild *b, const char *name);
int ed_panel_locked(int right);

// ---- editor_triggers.c (doc/v7/editor.md WP4): the logic UI of TRIGGER / AI_SPAWNER objects.
// ed_triggers_panel emits v7's "Trigger Event" and "Trigger Actions" folders (ui/editor.js
// UI:4080-4500) into the object panel; editor_inspector.c calls it inside "Technical & Logic".
#define ED_HAVE_TRIGGERS_PANEL 1
void ed_triggers_panel(EdBuild *b, EditorObject *o);
// v7's Custom Message gate: the TRIGGER's event is `event_name` ("onInteractKey", "onHoldMouse")
int ed_triggers_event_is(const EditorObject *o, const char *event_name);
