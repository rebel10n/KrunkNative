// A Krunker map document of kn_editor_mcp: the map JSON itself (cJSON, key order and unknown
// fields kept, so a save round-trips byte for byte), stable session ids for its objects, and a
// snapshot undo/redo history.
//
// Object ids: every object node of root.objects carries its id in cJSON's (otherwise unused for
// objects) valueint, assigned 1..N on load and next_uid for new objects. Ids are session-local,
// never written to the map. Live editor projects use the editor's objUUIDs instead.
#pragma once
#include <cJSON.h>
#include <limits.h>
#include <stddef.h>

typedef struct {
    char *text;   // compact map JSON before the edit
    int *uids;    // object ids in order
    int uid_count;
    char label[128];
    double time;
    int next_uid;
} DocHistEntry;

typedef struct {
    DocHistEntry *items;
    int count, cap;
} DocHist;

typedef struct Doc {
    cJSON *root;
    char path[PATH_MAX];  // resolved file path, "" when never saved
    int dirty;
    int next_uid;
    DocHist undo, redo;
    size_t history_bytes;
    int source_pretty;    // the loaded file was indented
    int trailing_newline; // the loaded file ended with "\n"
    int revision;         // bumps on every change (undo/redo included)
    int editor_form;      // live mirror: spawns / camera are SPAWN_POINT / CAMERA_POSITION objects
} Doc;

Doc *doc_from_text(const char *text, size_t len, char *err, size_t errsz);
// A blank map: "blank" (floor + 2 spawns), "empty" (no objects), "arena" (walled floor,
// 4 spawns per team). NULL on unknown template.
Doc *doc_from_template(const char *name, const char *map_name);
void doc_free(Doc *doc);

cJSON *doc_objects(Doc *doc); // root.objects, created when missing
int doc_object_count(Doc *doc);
cJSON *doc_object_at(Doc *doc, int index);
cJSON *doc_object_by_id(Doc *doc, int id, int *index_out);
int doc_object_id(const cJSON *obj);
// Appends (index < 0) or inserts obj (taken over) with a fresh id; returns the id.
int doc_add_object(Doc *doc, cJSON *obj, int index);
int doc_remove_object(Doc *doc, int id);    // 0 ok, -1 unknown id
void doc_assign_missing_ids(Doc *doc);

// ---- object semantics (raw keys -> values)
int obj_prefab(const cJSON *obj);                         // i / id (name or number); unknown -> CUBE like map.c
int obj_prefab_raw_valid(const cJSON *obj);               // 0 when i/id names no prefab
int obj_pos(const cJSON *obj, double out[3]);             // 0 when missing (out = 0)
void obj_size(const Doc *doc, const cJSON *obj, double out[3]); // s, xyz[si], else the prefab default
void obj_rot(const cJSON *obj, double out[3]);
// World bounds of the rotated box (rotation about the box centre, Euler XYZ like three.js).
void obj_bounds(const Doc *doc, const cJSON *obj, double mn[3], double mx[3]);
// Collision box as the shared physics sees it (axis aligned, rotation ignored): p.y is the bottom.
void obj_aabb(const Doc *doc, const cJSON *obj, double mn[3], double mx[3]);
// "#rrggbb" of c/ci (key "c") or e/ei ("e"); 0 when unset (out = the default).
int obj_color(const Doc *doc, const cJSON *obj, const char *key, char out[8]);
int obj_texture(const cJSON *obj);                         // texture index (t name or number), prefab default
int obj_collidable(const cJSON *obj);
int obj_visible(const cJSON *obj);
void obj_set_vec(cJSON *obj, const char *key, const double v[3]);
// Normalises "#RGB", "#rrggbb", "rrggbb", 0xRRGGBB numbers; 0 on failure.
int color_parse(const cJSON *v, char out[8]);

// ---- properties (schema.h); SET_FORCE: allow keys the prefab does not list; SET_CLAMP: clamp
// out-of-range numbers instead of failing. 0 on success, else err has the reason.
enum { SET_FORCE = 1, SET_CLAMP = 2, SET_RAW_UNKNOWN = 4 };
int doc_set_prop(Doc *doc, cJSON *obj, const char *name, const cJSON *value, int flags, char *err, size_t errsz);
int doc_unset_prop(cJSON *obj, const char *name);         // removes key + alt / palette keys; 1 if removed
// Friendly decoded properties of obj {prop: value}; with defaults also unset props the prefab allows.
cJSON *doc_props_json(const Doc *doc, const cJSON *obj, int with_defaults);
// The object as tools return it. detail 0 summary, 1 full (+ raw + props + bounds).
cJSON *doc_object_json(const Doc *doc, const cJSON *obj, int index, int detail);

// ---- history (file projects)
char *doc_snapshot(Doc *doc, int **uids, int *count);    // compact text + ids
int doc_restore(Doc *doc, const char *text, const int *uids, int count, int next_uid);
void doc_history_push(Doc *doc, const char *label, char *text, int *uids, int count, int next_uid); // takes ownership
int doc_undo(Doc *doc, char *label, size_t n);           // 1 when a step was undone
int doc_redo(Doc *doc, char *label, size_t n);
cJSON *doc_history_json(const Doc *doc, int limit);
void doc_history_clear(Doc *doc);

// The map as the game / the native tools read it: a duplicate of root with editor-form tool
// objects (live projects: SPAWN_POINT objects, CAMERA_POSITION, objUUID keys) converted to
// spawns / camPos, and spawns / objects arrays present. Caller deletes.
cJSON *doc_map_form(const Doc *doc);
