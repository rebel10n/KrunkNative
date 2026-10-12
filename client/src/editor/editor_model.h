// Map editor data model, GL-free (doc/v7/editor.md WP1): Krunker v7.1.4
// libs/ObjectInstance.js (constructor + serialize) and editor.js getMapExport / importMap /
// loadObjects / save, as pure functions over cJSON. editor_objects.c and editor_io.c build the
// editor on top of it; editor_serialize_test and kn_editor_mcp link it without GL.
//
// Object form: an editor object is kept as the JSON ObjectInstance.serialize() writes, colours
// and sizes inline (c / e / s, never ci / ei / si). ed_model_object() is v7's
// `ObjectInstance.deserialize(o).serialize()` (constructor defaults, legacy keys, prefab gates,
// rounding), so every object the editor holds is already in its exported form.
//
// Intentional differences from v7.1.4 (doc/v7/editor.md section 1.13):
// - Object keys v7's ObjectInstance never reads (e.g. v3.1.1 "dyn" / "gy") are kept after the v7
//   keys instead of being dropped, so maps round-trip without loss.
// - An invalid texture name or index loads as DEFAULT; v7 throws and the import fails.
// - Non-scalable model prefabs (no prefab defaultSize, or an asset model) keep the imported "s":
//   v7 recomputes it from the loaded model's bounds, which needs the model.
// - "sci" is written from the object's script id (v7 writes this.sci, i.e. null).
// - ENVIRONMENT_ZONE "skyDomeEmis" keeps its colour (v7 tests the output object and always writes
//   "#000000").
// - Select-type server settings are written as their option index (v7 writes NaN, i.e. null).
// - Imported "colors" / "xyz" are not kept as map keys (v7 re-exports a stale palette when the
//   map no longer has coloured objects).
// - Scripts are decoded from base64 as UTF-8 (v7 decodes as ASCII).
#pragma once
#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>
#include "editor_data.h"

// ---- data (editor_data.c's JSON blob, parsed once)
const cJSON *editor_data_json(void);
const cJSON *editor_data_get(const char *key);

// ---- objects (ObjectInstance)
// The prefab id of a serialized object: "i" / "id" as a number, numeric string or prefab name
// (getPrefabIdByName, case-insensitive). -1 = no such prefab ("Invalid Object").
int ed_model_prefab(const cJSON *obj);
// ObjectInstance.deserialize(obj).serialize(): a new object in the editor form; meshUUID is
// dropped. NULL when the prefab is invalid. Flags:
//   ED_MODEL_KEEP_UUID        objUUID is kept (save files)
//   ED_MODEL_EXACT_TRANSFORM  p / r are not rounded: the editor holds the exact transform like the
//                             three.js bounding mesh; getMapExport rounds objects (serialize) but
//                             writes camPos / spawns exact, as v7 does
enum { ED_MODEL_KEEP_UUID = 1, ED_MODEL_EXACT_TRANSFORM = 2 };
cJSON *ed_model_object(const cJSON *obj, int flags);
// The object keys ObjectInstance reads or writes (legacy ones included); 0 = unknown to v7.
int ed_model_known_key(const char *key);

// ---- the map
typedef struct {
    cJSON *map_config;    // mapConfig: every v7 default (Editor.defineDefaults order) + imported values
    cJSON *extra;         // imported top-level keys that are not mapConfig defaults (kept, exported after)
    cJSON *server_config; // {modes:{}, classes:{}, settings:{}, weps:{}, cls:{}, prks:{}} (index-keyed)
    cJSON *gui;           // custom GUI elements (EditorOverlay.customGUIs, parseGUIs(.., true) form)
    cJSON *scripts;       // {"client": code, "server": code}, decoded ("" = none)
} EdMapState;

void ed_model_map_init(EdMapState *s);   // the defaults of a new map (defineDefaults)
void ed_model_map_free(EdMapState *s);
int ed_model_map_key_known(const char *key); // a mapConfig default key

// One editor object for the export: its serialized form (colours / sizes inline).
typedef struct {
    const cJSON *data;
    int prefab;
    uint32_t uuid;
} EdModelObj;

enum {
    ED_EXPORT_RAW = 1,          // getMapExport(true): save form: spawns / cameras stay objects, objUUIDs
    ED_EXPORT_NO_SIZE_POOL = 2, // sizes stay inline "s" (the editor bus mirror); v7 always pools
    ED_EXPORT_NO_TRIS = 4,      // skip the complex-collision bake
};
// getMapExport: the map document (owned by the caller).
cJSON *ed_model_export(const EdMapState *s, const EdModelObj *objs, size_t n, int flags);

// importMap: `doc` is a map or a wrapper ({states | id | map}: a save file / hub record, .map
// used, a JSON string member parsed). The state is reset to the defaults and filled; *objects
// gets the map's objects (editor form, objUUIDs of a save kept), spawns and camPos as
// SPAWN_POINT / CAMERA_POSITION objects. *map_out (optional) is the map member used (borrowed
// from doc, or NULL when it was a string). Returns 0 when doc holds no map.
int ed_model_import(const cJSON *doc, EdMapState *s, cJSON **objects, const cJSON **map_out);

// ---- properties by ObjectInstance name ("visible", "canWallJump", "aiName", ...): read with the
// constructor's load rule; a set writes the key and re-derives the object with ed_model_object(),
// so serialize()'s gates and rounding apply. Props marked as action props ("action", "constant",
// "constantTxt", "snd", "vol", ...) live in tas[0] on TRIGGER / AI_SPAWNER. The transform, texture,
// faces, collidable, health, weaponId, mSize and bounce are handled by editor_objects.c.
cJSON *ed_model_prop_get(const cJSON *data, int prefab, const char *prop); // NULL = unknown prop
int ed_model_prop_set(cJSON **data, int prefab, const char *prop, const cJSON *value); // *data replaced; 0 = unknown
const char *ed_model_prop_key(const char *prop);                           // the JSON key, NULL = unknown

// ---- trigger actions ("tas", at most triggerActionLimit; action 0 cannot be removed)
int ed_model_action_count(const cJSON *data);
int ed_model_action_add(cJSON **data);                     // 0 at the limit
int ed_model_action_remove(cJSON **data, int index);       // index >= 1
cJSON *ed_model_action_get(const cJSON *data, int index, const char *key); // NULL = unset
int ed_model_action_set(cJSON **data, int prefab, int index, const char *key, const cJSON *value);

// ---- complex collision (getMapExport f967): world-space triangles of a "cpx" object appended
// to tris (9 numbers each, rounded to integers). Returns the triangles added; 0 for prefabs
// whose geometry needs a model (CUSTOM_ASSET).
int ed_model_object_tris(const cJSON *obj, int prefab, cJSON *tris);

// ---- JSON.stringify output (Number#toString numbers, JSON.stringify string escapes), malloc'd
char *ed_model_print(const cJSON *item);
void ed_model_number(double value, char *out, size_t size); // one number, >= 32 bytes

// ---- helpers shared with the editor
char *ed_model_b64_encode(const char *text);       // UTILS.b64e (UTF-8), malloc'd
char *ed_model_b64_decode(const char *b64);        // UTF-8; NULL when not base64 (b64Test)
const char *ed_model_script_template(const char *key); // KrunkScript client / server template
cJSON *ed_model_parse_guis(const cJSON *list, int keep_all); // UTILS.parseGUIs
