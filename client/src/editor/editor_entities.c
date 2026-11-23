// Map editor entities and archetypes: Krunker v10.0.7's editor (editor-10.0.7.js:18213-18851, the
// menus at index-10.0.7.js:6338-6343, the ENTITY panel buttons at index-10.0.7.js:7006-7020).
//
// An archetype is a named set of parts (objects relative to their centre) kept in the map's
// top-level "archetypes" list (EditorProject.extra_map, parsed with ed_model_parse_archetypes); an
// ENTITY object (prefab 98) places one by id ("ar") with its own entity id ("eid"), tag, auto
// spawn ("as") and spawn params ("sp"). The editor:
// - Object > Group > Save as Archetype: the selected group's objects become a new archetype;
// - Object > Entity > Place / Edit Selected / Archetypes, the ENTITY panel's Edit Entity / Edit
//   Archetype: the entity popup, the archetype popup (name, collision, health, parts with their
//   names and soft zones, data JSON), Edit Parts (the entity is swapped for its parts as a group;
//   Apply Parts writes them back), Unpack Parts, Delete;
// - the archetype browser and the stage: a plate at (0, 5000, 0), everything within 600 of it is the
//   archetype being built; Save / Cancel in a bar over the view;
// - "archetypes" history entries (editor_objects.c, kind 'A'), the ENTITY previews (editor_render.c
//   draws the parts as boxes inside the entity, its own volume fainter), the export leaving the stage
//   out (editor_io.c), dangling / stripped entity instances in prefabs (editor_tools.c).
//
// The texts are v10's English translations (editor.entity.*, editor-viewstack-10.0.7.js:2478).
// window.prompt() is the editor's in-page prompt (editor_prompt.c), so the steps after a name
// prompt run when it is answered.
#include "editor_internal.h"
#include "editor_model.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// editor.entity.* (en)
#define T_TITLE "Entity"
#define T_PLACE_TITLE "Place Entity"
#define T_PLACE "Place"
#define T_ARCHETYPE "Archetype"
#define T_EID "Entity ID"
#define T_TAG "Tag"
#define T_AUTO_SPAWN "Spawn on map load"
#define T_SPAWN_PARAMS "Spawn Params (JSON)"
#define T_SAVE "Save"
#define T_EDIT_ARCHETYPE "Edit Archetype"
#define T_SELECT_FIRST "Select an entity first."
#define T_BAD_JSON "That JSON is not a valid object."
#define T_LIMIT "Entity limit reached."
#define T_NO_ARCHETYPES "No archetypes yet. Group some objects and use Save as Archetype."
#define T_NO_GROUP "Select a group to save as an archetype."
#define T_NAME_PROMPT "Name your Archetype"
#define T_NAME_REQUIRED "Please name your Archetype"
#define T_ARCHETYPE_TITLE "Archetype"
#define T_ARCHETYPE_NAME "Name"
#define T_ARCHETYPE_LIMIT "Archetype limit reached."
#define T_ARCHETYPE_OBJECT_LIMIT "That group has too many objects for an archetype."
#define T_ARCHETYPE_IN_USE "%d entities still use this archetype."
#define T_REMOVE_ARCHETYPE "Delete"
#define T_UNPACK "Unpack Parts"
#define T_EDIT_PARTS "Edit Parts"
#define T_APPLY_PARTS "Apply Parts"
#define T_CANCEL_PARTS "Cancel Edit"
#define T_EDITING_PARTS "Editing parts — move, resize, add or remove objects in the group, then Apply Parts from the Entity menu."
#define T_EDIT_ACTIVE "A part edit is already in progress — Apply or Cancel it first."
#define T_EDIT_LOST "The part-edit group is gone — edit cancelled."
#define T_BROWSER_TITLE "Archetypes"
#define T_NEW_ARCHETYPE "New Archetype"
#define T_PROPERTIES "Properties"
#define T_DUPLICATE "Duplicate"
#define T_NO_ARCHETYPES_YET "No archetypes yet — build one on the stage."
#define T_STAGE_EDITING "Editing archetype: "
#define T_STAGE_NEW "New archetype"
#define T_STAGE_HINT "Build the parts on the stage below — everything near it becomes the archetype. Part names and soft zones are set in Properties after saving."
#define T_STAGE_ACTIVE "Finish the archetype stage first (Save or Cancel)."
#define T_NO_STAGE_PARTS "The stage is empty — add some objects first."
#define T_PICK_POINT "Pick point"
#define T_PREFAB_STRIPPED "%d entity instance(s) were left out of the prefab (prefabs cannot carry archetypes)."
#define T_PREFAB_DANGLING "%d imported entity instance(s) were dropped — their archetype does not exist in this map."
#define T_COLL "Collision"
#define T_HP "Health"
#define T_DATA "Archetype Data (JSON)"
#define T_PARTS "Parts"
#define T_PART_NAME "name"
#define T_SOFT_ZONE "soft"
#define T_NO_NESTED "Archetypes cannot contain entities."
static const char *const k_coll[3] = {"None", "Solid", "Hittable only"}; // editor.entity.coll0..2

static const struct { float x, y, z, radius; } k_stage = {0, 5000, 0, 600}; // editor-10.0.7.js:14216-14221

// KE.archEdit / KE.partEdit / pickPoint's pending click, for the project they were started in
static struct {
    int proj, active, ar_id; // ar_id 0 = a new archetype (archEdit.arId null)
    vec3 cam;
    float yaw, pitch;
    size_t undo_len;
    uint32_t plate;
} g_arch;
static struct {
    int proj, active, ar_id;
    cJSON *inst;
    uint32_t group;
} g_part;
static struct {
    int active, sp, ar_id;
    cJSON *json;
} g_pick;
static cJSON *g_pending_parts; // saveArchetype: the centred group while the name prompt is open

static void popup(const char *text) { ed_response_popup(NULL, text, NULL); }

static int proj_id(void) { return g_ed.proj ? g_ed.proj->id : 0; }
static int arch_active(void) { return g_arch.active && g_arch.proj == proj_id(); }
static int part_active(void) { return g_part.active && g_part.proj == proj_id(); }

static int limit_of(const char *key, const int fallback) { return (int) ed_json_num(editor_data_get("limits"), key, fallback); }

static int entity_prefab(void) {
    for (int i = 0; i < EDITOR_PREFAB_COUNT; i++) if (editor_prefab_has(i, EP_ISENTITY)) return i;
    return -1;
}

static int is_entity(const EditorObject *o) { return o && editor_prefab_has(o->prefab, EP_ISENTITY); }
static int obj_int(const EditorObject *o, const char *key) { return (int) ed_json_num(o->data, key, 0); }

// ---------------------------------------------------------------------------
// KE.archetypes

static cJSON *archetypes(int create) {
    if (!g_ed.proj || !g_ed.proj->extra_map) return NULL;
    cJSON *a = cJSON_GetObjectItemCaseSensitive(g_ed.proj->extra_map, "archetypes");
    if (!cJSON_IsArray(a) && create) {
        cJSON_DeleteItemFromObjectCaseSensitive(g_ed.proj->extra_map, "archetypes");
        a = cJSON_AddArrayToObject(g_ed.proj->extra_map, "archetypes");
    }
    return cJSON_IsArray(a) ? a : NULL;
}

static int archetype_total(void) { return cJSON_GetArraySize(archetypes(0)); }

cJSON *ed_archetypes_snapshot(void) {
    const cJSON *a = archetypes(0);
    return a ? cJSON_Duplicate(a, 1) : cJSON_CreateArray();
}

void ed_archetypes_restore(const cJSON *list) {
    if (!g_ed.proj || !g_ed.proj->extra_map) return;
    cJSON *parsed = ed_model_parse_archetypes(list);
    cJSON_DeleteItemFromObjectCaseSensitive(g_ed.proj->extra_map, "archetypes");
    cJSON_AddItemToObject(g_ed.proj->extra_map, "archetypes", parsed);
    g_ed.dirty_counts = 1; // refreshEntityPreviews: updateObjectCount (the previews are drawn per frame)
}

const cJSON *ed_archetype_get(const int id) {
    // getArchetype(id): parseInt(id) || 0
    const cJSON *a;
    cJSON_ArrayForEach(a, archetypes(0)) if ((int) ed_json_num(a, "id", 0) == id) return a;
    return NULL;
}

static int next_archetype_id(void) {
    int top = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, archetypes(0)) if ((int) ed_json_num(a, "id", 0) > top) top = (int) ed_json_num(a, "id", 0);
    return top + 1;
}

static int next_entity_id(const EditorObject *skip) {
    int top = 0;
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        const EditorObject *o = g_ed.proj->objects[i];
        if (o != skip && is_entity(o) && obj_int(o, "eid") > top) top = obj_int(o, "eid");
    }
    return top + 1;
}

static int entities_using(const int ar) {
    int n = 0;
    for (size_t i = 0; i < g_ed.proj->count; i++) n += is_entity(g_ed.proj->objects[i]) && obj_int(g_ed.proj->objects[i], "ar") == ar;
    return n;
}

static int entity_total(void) {
    int n = 0;
    for (size_t i = 0; i < g_ed.proj->count; i++) n += is_entity(g_ed.proj->objects[i]);
    return n;
}

static void push_history(void) { ed_history_add_archetypes(ed_archetypes_snapshot()); }

void ed_entity_fix_id(EditorObject *o) {
    if (!is_entity(o) || !g_ed.proj) return;
    const int id = obj_int(o, "eid");
    int taken = !id;
    for (size_t i = 0; !taken && i < g_ed.proj->count; i++) {
        const EditorObject *x = g_ed.proj->objects[i];
        taken = x != o && is_entity(x) && obj_int(x, "eid") == id;
    }
    if (taken) ed_json_set_num(o->data, "eid", next_entity_id(o));
}

int ed_entity_ghost(const EditorObject *o) { return is_entity(o) && ed_archetype_get(obj_int(o, "ar")) != NULL; }

// ---------------------------------------------------------------------------
// helpers

// JSON.stringify(v, null, 1)
static void json1(EdBuf *b, const cJSON *v, const int depth) {
    if (cJSON_IsObject(v) || cJSON_IsArray(v)) {
        const int obj = cJSON_IsObject(v);
        if (!v->child) {
            edb_add(b, obj ? "{}" : "[]");
            return;
        }
        edb_add(b, obj ? "{\n" : "[\n");
        for (const cJSON *c = v->child; c; c = c->next) {
            for (int i = 0; i <= depth; i++) edb_add(b, " ");
            if (obj) {
                cJSON *k = cJSON_CreateString(c->string ? c->string : "");
                char *ks = cJSON_PrintUnformatted(k);
                edb_add(b, ks ? ks : "\"\"");
                edb_add(b, ": ");
                free(ks);
                cJSON_Delete(k);
            }
            json1(b, c, depth + 1);
            edb_add(b, c->next ? ",\n" : "\n");
        }
        for (int i = 0; i < depth; i++) edb_add(b, " ");
        edb_add(b, obj ? "}" : "]");
    } else if (cJSON_IsNumber(v)) {
        char n[48];
        if (!isfinite(v->valuedouble)) snprintf(n, sizeof(n), "null");
        else ed_model_num_str(v->valuedouble, n, sizeof(n));
        edb_add(b, n);
    } else {
        char *s = cJSON_PrintUnformatted(v);
        edb_add(b, s ? s : "null");
        free(s);
    }
}

static char *json1_str(const cJSON *v) {
    EdBuf b;
    edb_init(&b);
    json1(&b, v, 0);
    return b.buf;
}

static void add_json1_escaped(EdBuf *b, const cJSON *v) {
    char *s = json1_str(v);
    edb_add_escaped(b, s ? s : "{}");
    free(s);
}

enum { VALUE_MAX = 1 << 16 };

static char *value_of(const HtmlElem e) {
    char *buf = calloc(1, VALUE_MAX);
    if (buf && e) html_dom_get_value(e, buf, VALUE_MAX);
    return buf;
}

static int id_value_int(const char *id, const int fallback) {
    const HtmlElem e = html_dom_by_id(id);
    if (!e) return fallback;
    char buf[64] = "";
    html_dom_get_value(e, buf, sizeof(buf));
    return (int) strtol(buf, NULL, 10);
}

// checkEntityJSON(el): the field's JSON object ("" = {}), the border red when it is not one
static cJSON *check_json(const HtmlElem e) {
    if (!e) return NULL;
    char *text = value_of(e);
    if (!text) return NULL;
    char *s = text;
    while (isspace((unsigned char) *s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char) s[n - 1])) s[--n] = 0;
    cJSON *v = cJSON_ParseWithOpts(n ? s : "{}", NULL, 1);
    free(text);
    if (!cJSON_IsObject(v)) {
        cJSON_Delete(v);
        html_dom_set_style(e, "border-color", "#e33b3b");
        return NULL;
    }
    html_dom_set_style(e, "border-color", "");
    return v;
}

// recordsToParts / partsToRecords (editor-10.0.7.js:18494-18519): a part's name is "n" and its soft
// zone "soft" in the archetype, "pn" / "sfz" on an object
static int part_name_ok(const char *s) {
    const size_t n = strlen(s);
    if (n < 1 || n > 12) return 0;
    for (const char *c = s; *c; c++) if (!isalnum((unsigned char) *c) && *c != '_') return 0;
    return 1;
}

static void records_to_parts(cJSON *arr) {
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        const cJSON *pn = cJSON_GetObjectItemCaseSensitive(o, "pn");
        char buf[48] = "";
        if (cJSON_IsString(pn)) snprintf(buf, sizeof(buf), "%s", pn->valuestring);
        else if (cJSON_IsNumber(pn) && pn->valuedouble != 0) ed_model_num_str(pn->valuedouble, buf, sizeof(buf));
        if (buf[0] && part_name_ok(buf)) ed_json_set_str(o, "n", buf);
        cJSON_DeleteItemFromObjectCaseSensitive(o, "pn");
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(o, "sfz"))) ed_json_set_num(o, "soft", 1);
        cJSON_DeleteItemFromObjectCaseSensitive(o, "sfz");
    }
}

static cJSON *parts_to_records(const cJSON *parts) {
    cJSON *arr = cJSON_Duplicate(parts, 1);
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "n");
        if (cJSON_IsString(n)) {
            ed_json_set_str(o, "pn", n->valuestring);
            cJSON_DeleteItemFromObjectCaseSensitive(o, "n");
        }
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(o, "soft"))) ed_json_set_num(o, "sfz", 1);
        cJSON_DeleteItemFromObjectCaseSensitive(o, "soft");
    }
    return arr;
}

static double pnum(const cJSON *o, const char *key, const int i) {
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, key), i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void set_pnum(cJSON *o, const char *key, const int i, const double v) {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsArray(a)) return;
    cJSON_ReplaceItemInArray(a, i, cJSON_CreateNumber(v));
}

// applyCenter (editor-10.0.7.js:17931-17939)
static void apply_center(cJSON *arr) {
    if (!cJSON_GetArraySize(arr)) return;
    double c[6];
    ed_find_center(arr, c);
    cJSON *o;
    cJSON_ArrayForEach(o, arr) for (int k = 0; k < 3; k++) set_pnum(o, "p", k, pnum(o, "p", k) - c[k]);
}

static int has_entity_record(const cJSON *arr) {
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        const cJSON *eid = cJSON_GetObjectItemCaseSensitive(o, "eid");
        if (eid && !cJSON_IsNull(eid)) return 1; // _17.eid != null
    }
    return 0;
}

static int js_null(const cJSON *v) { return !v || cJSON_IsNull(v); } // v == null

static int record_ar(const cJSON *o) {
    const cJSON *ar = cJSON_GetObjectItemCaseSensitive(o, "ar");
    if (cJSON_IsNumber(ar)) return (int) ar->valuedouble;
    return cJSON_IsString(ar) ? (int) strtol(ar->valuestring, NULL, 10) : 0; // parseInt(ar) || 0
}

int ed_entity_drop_dangling(cJSON *records) {
    int dropped = 0;
    for (int i = cJSON_GetArraySize(records) - 1; i >= 0; i--) {
        const cJSON *o = cJSON_GetArrayItem(records, i);
        const int plain = js_null(cJSON_GetObjectItemCaseSensitive(o, "eid")) && js_null(cJSON_GetObjectItemCaseSensitive(o, "ar"));
        if (plain || ed_archetype_get(record_ar(o))) continue;
        cJSON_DeleteItemFromArray(records, i);
        dropped++;
    }
    return dropped;
}

int ed_entity_strip_instances(cJSON *records) {
    int stripped = 0;
    for (int i = cJSON_GetArraySize(records) - 1; i >= 0; i--) {
        if (js_null(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(records, i), "eid"))) continue;
        cJSON_DeleteItemFromArray(records, i);
        stripped++;
    }
    return stripped;
}

static cJSON *new_archetype(const int id, const char *name, cJSON *objects) {
    cJSON *a = cJSON_CreateObject();
    char clean[32];
    ed_model_archetype_name(name, clean, sizeof(clean));
    cJSON_AddNumberToObject(a, "id", id);
    cJSON_AddStringToObject(a, "name", clean);
    cJSON_AddItemToObject(a, "objects", objects);
    cJSON_AddNumberToObject(a, "coll", 1);
    cJSON_AddNumberToObject(a, "hp", 0);
    cJSON_AddObjectToObject(a, "data");
    return a;
}

static void archetype_label(EdBuf *b, const cJSON *a) {
    // esc(a.name || "Archetype " + a.id)
    const char *name = ed_json_str(a, "name", "");
    if (name[0]) edb_add_escaped(b, name);
    else edb_addf(b, "Archetype %d", (int) ed_json_num(a, "id", 0));
}

static void archetype_options(EdBuf *b, const int selected) {
    const cJSON *a;
    cJSON_ArrayForEach(a, archetypes(0)) {
        const int id = (int) ed_json_num(a, "id", 0);
        edb_addf(b, "<option value='%d'%s>", id, id == selected ? " selected='selected'" : "");
        archetype_label(b, a);
        edb_add(b, "</option>");
    }
}

static void stage_bar_remove(void) {
    const HtmlElem bar = html_dom_by_id("archStageBar");
    if (bar) html_dom_remove(bar);
}

// ---------------------------------------------------------------------------
// saveArchetype (editor-10.0.7.js:18258-18287)

static void archetype_popup(int id);

static void save_archetype_named(void *user, const char *name) {
    (void) user;
    cJSON *parts = g_pending_parts;
    g_pending_parts = NULL;
    if (!parts) return;
    if (!name || !*name) {
        cJSON_Delete(parts);
        popup(T_NAME_REQUIRED);
        return;
    }
    push_history();
    const int id = next_archetype_id();
    apply_center(parts);
    records_to_parts(parts);
    cJSON_AddItemToArray(archetypes(1), new_archetype(id, name, parts));
    g_ed.dirty_counts = 1;
    ed_mark_changed();
    archetype_popup(id);
}

void ed_entity_save_archetype(void) {
    if (archetype_total() >= limit_of("archetypeLimit", 32)) {
        popup(T_ARCHETYPE_LIMIT);
        return;
    }
    const cJSON *group = ed_copy_group(NULL);
    if (!group || !cJSON_GetArraySize(group)) {
        popup(T_NO_GROUP);
        return;
    }
    if (cJSON_GetArraySize(group) > limit_of("archetypeObjectLimit", 40)) {
        popup(T_ARCHETYPE_OBJECT_LIMIT);
        return;
    }
    if (has_entity_record(group)) {
        popup(T_NO_NESTED);
        return;
    }
    cJSON_Delete(g_pending_parts);
    g_pending_parts = cJSON_Duplicate(group, 1);
    ed_prompt(T_NAME_PROMPT, "", save_archetype_named, NULL);
}

// ---------------------------------------------------------------------------
// placeEntity / createEntity (editor-10.0.7.js:18288-18320)

void ed_entity_place(void) {
    if (!archetype_total()) {
        popup(T_NO_ARCHETYPES);
        return;
    }
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "<div class='pubHed'>" T_PLACE_TITLE "</div><select id='entityArPick' class='inputGrey2'>");
    archetype_options(&b, 0);
    edb_add(&b, "</select><div class='eButton teleBtn' style='margin-top:10px;' onclick='KE.createEntity()'>" T_PLACE "</div>");
    ed_generic_popup(b.buf, "entityPop");
    edb_free(&b);
}

static void create_entity(int ar) {
    if (!ar) ar = id_value_int("entityArPick", 0);
    if (!ed_archetype_get(ar)) {
        popup(T_NO_ARCHETYPES);
        return;
    }
    if (entity_total() >= limit_of("entityLimit", 64)) {
        popup(T_LIMIT);
        return;
    }
    const vec3 p = editor_setting_bool("createObjectNear") ? editor_near_position() : (vec3) {0};
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "i", entity_prefab());
    cJSON *pa = cJSON_AddArrayToObject(d, "p");
    cJSON_AddItemToArray(pa, cJSON_CreateNumber(ed_round(p.x, 2)));
    cJSON_AddItemToArray(pa, cJSON_CreateNumber(ed_round(p.y, 2)));
    cJSON_AddItemToArray(pa, cJSON_CreateNumber(ed_round(p.z, 2)));
    cJSON_AddNumberToObject(d, "eid", next_entity_id(NULL));
    cJSON_AddNumberToObject(d, "ar", ar);
    EditorObject *o = ed_obj_new(d, 0); // defaultFromType(ENTITY, pos); entityId, archetypeId
    cJSON_Delete(d);
    ed_obj_add(o, 0, 1);
    ed_clear_pops();
    ed_entity_popup();
}

// ---------------------------------------------------------------------------
// entityPopup / saveEntity (editor-10.0.7.js:18321-18333, 18407-18433)

void ed_entity_popup(void) {
    const EditorObject *o = g_ed.proj ? g_ed.proj->selected : NULL;
    if (!is_entity(o)) {
        if (part_active()) archetype_popup(g_part.ar_id);
        else popup(T_SELECT_FIRST);
        return;
    }
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "<div class='pubHed'>" T_TITLE "</div><div style='margin-top:12px;font-size:18px;color:#fff'>" T_ARCHETYPE
                "</div><select id='entityAr' class='inputGrey2'>");
    archetype_options(&b, obj_int(o, "ar"));
    edb_addf(&b, "</select><div style='margin-top:12px;font-size:18px;color:#fff'>" T_EID "</div><input id='entityEid' class='inputGrey2' disabled value='%d'>"
                 "<div style='margin-top:12px;font-size:18px;color:#fff'>" T_TAG "</div><input id='entityTag' class='inputGrey2' maxlength='32' value='",
             obj_int(o, "eid"));
    edb_add_escaped(&b, ed_json_str(o->data, "tag", ""));
    const cJSON *as = cJSON_GetObjectItemCaseSensitive(o->data, "as");
    const int auto_spawn = !as || cJSON_IsNull(as) || ed_json_truthy(as); // as == null ? true : !!parseInt(as)
    edb_addf(&b, "'><div style='margin-top:12px;font-size:18px;color:#fff'><input id='entityAuto' type='checkbox' %s> " T_AUTO_SPAWN
                 "</div><div style='margin-top:12px;font-size:18px;color:#fff'>" T_SPAWN_PARAMS
                 " <span class='eButton teleBtn' style='margin:0 0 0 8px;padding:2px 8px;font-size:13px;display:inline-block;' onclick='KE.pickPoint(\"sp\")'>" T_PICK_POINT
                 "</span></div><textarea id='entitySp' class='inputGrey2' style='float:none;width:calc(100%% - 26px);resize:vertical;font-family:monospace;' rows='5' "
                 "onblur='KE.checkEntityJSON(this)'>",
             auto_spawn ? "checked='checked' " : "");
    const cJSON *sp = cJSON_GetObjectItemCaseSensitive(o->data, "sp");
    cJSON *empty = cJSON_CreateObject();
    add_json1_escaped(&b, cJSON_IsObject(sp) ? sp : empty); // spawnParams || {}
    cJSON_Delete(empty);
    edb_add(&b, "</textarea><div class='eButton teleBtn' style='margin-top:10px;' onclick='KE.saveEntity()'>" T_SAVE
                "</div><div class='eButton teleBtn' style='margin-top:10px;margin-left:14px;' onclick='KE.archetypePopup()'>" T_EDIT_ARCHETYPE "</div>");
    ed_generic_popup(b.buf, "entityPop");
    edb_free(&b);
}

static void save_entity(void) {
    EditorObject *o = g_ed.proj ? g_ed.proj->selected : NULL;
    if (!is_entity(o)) {
        ed_clear_pops();
        return;
    }
    cJSON *sp = check_json(html_dom_by_id("entitySp"));
    if (!sp) {
        popup(T_BAD_JSON);
        return;
    }
    const HtmlElem ar = html_dom_by_id("entityAr"), tag = html_dom_by_id("entityTag"), autos = html_dom_by_id("entityAuto");
    if (ar && ed_archetype_get(id_value_int("entityAr", 0))) ed_json_set_num(o->data, "ar", id_value_int("entityAr", 0));
    if (tag) {
        char raw[256] = "", clean[33];
        html_dom_get_value(tag, raw, sizeof(raw));
        size_t j = 0;
        for (const char *c = raw; *c && j < 32; c++) if (isalnum((unsigned char) *c) || *c == '_' || *c == '-') clean[j++] = *c;
        clean[j] = 0;
        if (j) ed_json_set_str(o->data, "tag", clean);
        else cJSON_DeleteItemFromObjectCaseSensitive(o->data, "tag"); // serialize writes a non-empty tag only
    }
    if (autos) {
        if (html_dom_get_checked(autos)) cJSON_DeleteItemFromObjectCaseSensitive(o->data, "as");
        else ed_json_set_num(o->data, "as", 0);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(o->data, "sp");
    if (sp->child) cJSON_AddItemToObject(o->data, "sp", sp); // Object.keys(sp).length ? sp : null
    else cJSON_Delete(sp);
    g_ed.dirty_panel = 1; // updateObjConfigGUI
    ed_mark_changed();
    ed_clear_pops();
}

// ---------------------------------------------------------------------------
// pickPoint (editor-10.0.7.js:18340-18386): the next pointerdown on the 3D view adds "ptN" to the
// spawn params / archetype data

static void pick_point(const int sp) {
    const HtmlElem field = html_dom_by_id(sp ? "entitySp" : "archetypeData");
    if (!field) return;
    cJSON *json = check_json(field);
    if (!json) {
        popup(T_BAD_JSON);
        return;
    }
    cJSON_Delete(g_pick.json);
    g_pick.json = json;
    g_pick.sp = sp;
    g_pick.ar_id = sp ? 0 : id_value_int("archetypeId", 0);
    g_pick.active = 1;
    ed_clear_pops();
}

static void pick_done(const vec3 pt) {
    g_pick.active = 0;
    cJSON *json = g_pick.json;
    g_pick.json = NULL;
    if (!json) return;
    int i = 1;
    char key[24];
    for (;; i++) {
        snprintf(key, sizeof(key), "pt%d", i);
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(json, key);
        if (!v || cJSON_IsNull(v)) break;
    }
    // [+x.toFixed(1), +y.toFixed(1), +z.toFixed(1)]
    const double xyz[3] = {round(pt.x * 10.0) / 10.0, round(pt.y * 10.0) / 10.0, round(pt.z * 10.0) / 10.0};
    cJSON_DeleteItemFromObjectCaseSensitive(json, key);
    cJSON_AddItemToObject(json, key, cJSON_CreateDoubleArray(xyz, 3));
    if (g_pick.sp) ed_entity_popup();
    else archetype_popup(g_pick.ar_id);
    const HtmlElem field = html_dom_by_id(g_pick.sp ? "entitySp" : "archetypeData");
    if (field) {
        char *s = json1_str(json);
        html_dom_set_value(field, s ? s : "{}");
        free(s);
        cJSON_Delete(check_json(field));
    }
    cJSON_Delete(json);
}

int ed_entity_pick_click(const double fb_x, const double fb_y) {
    if (!g_pick.active) return 0;
    EditorObject *o = NULL;
    vec3 hit;
    if (!ed_pick(fb_x, fb_y, 1, &o, &hit, NULL)) {
        vec3 origin, dir; // raycaster.ray.at(100)
        ed_view_ray(fb_x, fb_y, &origin, &dir);
        hit = (vec3) {origin.x + dir.x * 100.0f, origin.y + dir.y * 100.0f, origin.z + dir.z * 100.0f};
    }
    pick_done(hit);
    return 1;
}

// ---------------------------------------------------------------------------
// Edit Parts / Apply Parts / Cancel Edit / Unpack Parts (editor-10.0.7.js:18520-18609)

static char *parts_json(const cJSON *ar) {
    cJSON *rec = parts_to_records(cJSON_GetObjectItemCaseSensitive(ar, "objects"));
    char *s = cJSON_PrintUnformatted(rec);
    cJSON_Delete(rec);
    return s;
}

static void unpack_archetype(void) {
    const cJSON *ar = ed_archetype_get(id_value_int("archetypeId", 0));
    if (!ar) {
        ed_clear_pops();
        return;
    }
    EditorObject *sel = g_ed.proj->selected;
    if (!is_entity(sel)) {
        popup(T_SELECT_FIRST);
        return;
    }
    ed_clear_pops();
    ed_json_set_num(g_ed.advanced, "rotation", 0);
    char *s = parts_json(ar);
    ed_obj_replace(sel, s, NULL, 1); // replaceObject(sel, parts, true, false, true)
    free(s);
}

static void edit_entity_parts(void) {
    if (arch_active()) {
        popup(T_STAGE_ACTIVE);
        return;
    }
    if (part_active()) {
        popup(T_EDIT_ACTIVE);
        return;
    }
    EditorObject *sel = g_ed.proj->selected;
    if (!is_entity(sel)) {
        popup(T_SELECT_FIRST);
        return;
    }
    const cJSON *ar = ed_archetype_get(obj_int(sel, "ar"));
    if (!ar) {
        ed_clear_pops();
        return;
    }
    ed_clear_pops();
    cJSON_Delete(g_part.inst);
    g_part.inst = ed_obj_serialize(sel);
    g_part.ar_id = (int) ed_json_num(ar, "id", 0);
    g_part.proj = proj_id();
    g_part.active = 1;
    g_part.group = 0;
    ed_json_set_num(g_ed.advanced, "rotation", 0);
    const size_t groups = g_ed.proj->group_count;
    char *s = parts_json(ar);
    ed_obj_replace(sel, s, NULL, 1);
    free(s);
    // objectSelected(true): createGroup's addObject selects the new placeholder
    if (g_ed.proj->group_count > groups) {
        g_part.group = g_ed.proj->groups[g_ed.proj->group_count - 1].owner;
        ed_obj_select(ed_obj_find(g_part.group));
    }
    popup(T_EDITING_PARTS);
}

static void apply_entity_parts(void) {
    if (!part_active()) {
        ed_clear_pops();
        return;
    }
    cJSON *ar = (cJSON *) ed_archetype_get(g_part.ar_id);
    EditorObject *owner = g_part.group ? ed_obj_find(g_part.group) : NULL;
    const EditorGroup *grp = owner ? ed_group_of(owner) : NULL;
    if (!ar || !grp) {
        g_part.active = 0;
        popup(T_EDIT_LOST);
        return;
    }
    cJSON *records = cJSON_CreateArray();
    for (size_t i = 0; i < g_ed.proj->count; i++) {
        for (size_t k = 0; k < grp->count; k++) {
            if (grp->objects[k] == g_ed.proj->objects[i]->uuid) {
                cJSON_AddItemToArray(records, ed_obj_serialize(g_ed.proj->objects[i]));
                break;
            }
        }
    }
    if (!cJSON_GetArraySize(records)) {
        cJSON_Delete(records);
        popup(T_NO_GROUP);
        return;
    }
    if (cJSON_GetArraySize(records) > limit_of("archetypeObjectLimit", 40)) {
        cJSON_Delete(records);
        popup(T_ARCHETYPE_OBJECT_LIMIT);
        return;
    }
    if (has_entity_record(records)) {
        cJSON_Delete(records);
        popup(T_NO_NESTED);
        return;
    }
    double c[6];
    ed_find_center(records, c);
    push_history();
    apply_center(records);
    records_to_parts(records);
    ar = (cJSON *) ed_archetype_get(g_part.ar_id); // push_history copied the list; ar is still the live entry
    cJSON_ReplaceItemInObjectCaseSensitive(ar, "objects", records);
    ed_remove_group(owner);
    cJSON *inst = cJSON_Duplicate(g_part.inst, 1);
    const double p[3] = {c[0], c[1], c[2]};
    cJSON_DeleteItemFromObjectCaseSensitive(inst, "p");
    cJSON_AddItemToObject(inst, "p", cJSON_CreateDoubleArray(p, 3));
    EditorObject *o = ed_obj_new(inst, 0);
    cJSON_Delete(inst);
    ed_obj_add(o, 0, 1);
    g_part.active = 0;
    cJSON_Delete(g_part.inst);
    g_part.inst = NULL;
    g_ed.dirty_counts = 1; // refreshEntityPreviews
    ed_mark_changed();
    ed_clear_pops();
}

static void cancel_entity_parts(void) {
    const int active = part_active();
    g_part.active = 0;
    if (!active) {
        ed_clear_pops();
        return;
    }
    EditorObject *owner = g_part.group ? ed_obj_find(g_part.group) : NULL;
    if (owner && ed_group_of(owner)) ed_remove_group(owner);
    EditorObject *o = g_part.inst ? ed_obj_new(g_part.inst, 0) : NULL;
    ed_obj_add(o, 0, 1);
    cJSON_Delete(g_part.inst);
    g_part.inst = NULL;
    ed_clear_pops();
}

// ---------------------------------------------------------------------------
// archetypePopup / saveArchetypeData / removeArchetype (editor-10.0.7.js:18771-18851)

static void archetype_popup(int id) {
    if (id < 0) {
        const EditorObject *o = g_ed.proj ? g_ed.proj->selected : NULL;
        id = is_entity(o) ? obj_int(o, "ar") : part_active() ? g_part.ar_id : 0;
    }
    const cJSON *a = ed_archetype_get(id);
    if (!a) {
        popup(T_NO_ARCHETYPES);
        return;
    }
    const int aid = (int) ed_json_num(a, "id", 0);
    const int editing = part_active() && g_part.ar_id == aid;
    EdBuf b;
    edb_init(&b);
    edb_addf(&b, "<div class='pubHed'>" T_ARCHETYPE_TITLE "</div><input id='archetypeId' type='hidden' value='%d'><div style='margin-top:12px;font-size:18px;color:#fff'>"
                 T_ARCHETYPE_NAME "</div><input id='archetypeName' class='inputGrey2' maxlength='24' value='", aid);
    edb_add_escaped(&b, ed_json_str(a, "name", ""));
    edb_add(&b, "'><div style='margin-top:12px;font-size:18px;color:#fff'>" T_COLL "</div><select id='archetypeColl' class='inputGrey2'>");
    const int coll = (int) ed_json_num(a, "coll", 1);
    for (int i = 0; i < 3; i++) edb_addf(&b, "<option value='%d'%s>%s</option>", i, coll == i ? " selected='selected'" : "", k_coll[i]);
    edb_addf(&b, "</select><div style='margin-top:12px;font-size:18px;color:#fff'>" T_HP "</div><input id='archetypeHp' class='inputGrey2' type='number' min='0' value='%d'>"
                 "<div style='margin-top:12px;font-size:18px;color:#fff'>" T_PARTS "</div><div style='max-height:170px;overflow-y:auto;'>",
             (int) ed_json_num(a, "hp", 0));
    int i = 0;
    const cJSON *part;
    cJSON_ArrayForEach(part, cJSON_GetObjectItemCaseSensitive(a, "objects")) {
        edb_addf(&b, "<div style='display:flex;align-items:center;gap:8px;margin-top:4px;font-size:14px;color:#ccc'><span style='flex:1;text-align:left;white-space:nowrap;overflow:hidden;'>#%d ", i);
        const cJSON *aidv = cJSON_GetObjectItemCaseSensitive(part, "aid");
        if (aidv && !cJSON_IsNull(aidv)) {
            char nb[48];
            const char *s = cJSON_IsString(aidv) ? aidv->valuestring : NULL;
            if (!s && cJSON_IsNumber(aidv)) {
                ed_model_num_str(aidv->valuedouble, nb, sizeof(nb));
                s = nb;
            }
            edb_add(&b, "model ");
            edb_add_escaped(&b, s ? s : "");
        } else {
            const cJSON *v;
            int first = 1;
            cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(part, "s")) { // (s || []).join("&times;")
                char nb[48];
                ed_model_num_str(cJSON_GetNumberValue(v), nb, sizeof(nb));
                edb_addf(&b, "%s%s", first ? "" : "&times;", nb);
                first = 0;
            }
        }
        edb_addf(&b, "</span><input class='inputGrey2 archetypePartName' data-part='%d' maxlength='12' placeholder='" T_PART_NAME
                     "' style='float:none;margin:0;width:100px;padding:3px 6px;font-size:13px;' value='", i);
        edb_add_escaped(&b, ed_json_str(part, "n", ""));
        edb_addf(&b, "'><label style='display:flex;align-items:center;gap:4px;font-size:13px;cursor:pointer;'><input type='checkbox' class='archetypePartSoft' data-part='%d'%s>" T_SOFT_ZONE
                     "</label></div>",
                 i, ed_json_truthy(cJSON_GetObjectItemCaseSensitive(part, "soft")) ? " checked='checked'" : "");
        i++;
    }
    edb_add(&b, "</div><div style='margin-top:12px;font-size:18px;color:#fff'>" T_DATA
                " <span class='eButton teleBtn' style='margin:0 0 0 8px;padding:2px 8px;font-size:13px;display:inline-block;' onclick='KE.pickPoint(\"data\")'>" T_PICK_POINT
                "</span></div><textarea id='archetypeData' class='inputGrey2' style='float:none;width:calc(100% - 26px);resize:vertical;font-family:monospace;' rows='6' "
                "onblur='KE.checkEntityJSON(this)'>");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(a, "data");
    cJSON *empty = cJSON_CreateObject();
    add_json1_escaped(&b, cJSON_IsObject(data) ? data : empty);
    cJSON_Delete(empty);
    edb_add(&b, "</textarea><div class='eButton teleBtn' style='margin-top:10px;' onclick='KE.saveArchetypeData()'>" T_SAVE "</div>");
    if (editing)
        edb_add(&b, "<div class='eButton teleBtn' style='margin-top:10px;margin-left:14px;' onclick='KE.applyEntityParts()'>" T_APPLY_PARTS
                    "</div><div class='eButton teleBtn' style='margin-top:10px;margin-left:14px;' onclick='KE.cancelEntityParts()'>" T_CANCEL_PARTS "</div>");
    else
        edb_add(&b, "<div class='eButton teleBtn' style='margin-top:10px;margin-left:14px;' onclick='KE.editEntityParts()'>" T_EDIT_PARTS
                    "</div><div class='eButton teleBtn' style='margin-top:10px;margin-left:14px;' onclick='KE.unpackArchetype()'>" T_UNPACK "</div>");
    edb_add(&b, "<div class='eButton teleBtn' style='margin-top:10px;margin-left:14px;' onclick='KE.removeArchetype()'>" T_REMOVE_ARCHETYPE "</div>");
    ed_generic_popup(b.buf, "entityPop");
    edb_free(&b);
}

void ed_archetype_popup_selected(void) { archetype_popup(-1); }

static void save_archetype_data(void) {
    cJSON *a = (cJSON *) ed_archetype_get(id_value_int("archetypeId", 0));
    if (!a) {
        ed_clear_pops();
        return;
    }
    cJSON *data = check_json(html_dom_by_id("archetypeData"));
    if (!data) {
        popup(T_BAD_JSON);
        return;
    }
    const HtmlElem name = html_dom_by_id("archetypeName"), coll = html_dom_by_id("archetypeColl"), hp = html_dom_by_id("archetypeHp");
    push_history();
    a = (cJSON *) ed_archetype_get(id_value_int("archetypeId", 0));
    if (name) {
        char raw[256] = "", clean[32];
        html_dom_get_value(name, raw, sizeof(raw));
        ed_model_archetype_name(raw, clean, sizeof(clean));
        ed_json_set_str(a, "name", clean);
    }
    if (coll) {
        char v[16] = "";
        html_dom_get_value(coll, v, sizeof(v));
        char *end;
        const long c = strtol(v, &end, 10);
        if (end != v && c >= 0 && c <= 2) ed_json_set_num(a, "coll", (double) c);
    }
    if (hp) {
        const int h = id_value_int("archetypeHp", 0);
        ed_json_set_num(a, "hp", h > 0 ? h : 0); // Math.max(0, parseInt(hp) || 0)
    }
    cJSON_ReplaceItemInObjectCaseSensitive(a, "data", data);
    cJSON *objects = cJSON_GetObjectItemCaseSensitive(a, "objects");
    HtmlElem items[64];
    const HtmlElem pop = html_dom_by_id("genericPop");
    int n = html_dom_query_all(pop, ".archetypePartName", items, 64);
    for (int k = 0; k < n && k < 64; k++) {
        char idx[16] = "", v[64] = "";
        html_dom_get_attr(items[k], "data-part", idx, sizeof(idx));
        cJSON *part = cJSON_GetArrayItem(objects, atoi(idx));
        if (!part) continue;
        html_dom_get_value(items[k], v, sizeof(v));
        char *s = v;
        while (isspace((unsigned char) *s)) s++;
        size_t l = strlen(s);
        while (l && isspace((unsigned char) s[l - 1])) s[--l] = 0;
        if (part_name_ok(s)) ed_json_set_str(part, "n", s);
        else cJSON_DeleteItemFromObjectCaseSensitive(part, "n");
    }
    n = html_dom_query_all(pop, ".archetypePartSoft", items, 64);
    for (int k = 0; k < n && k < 64; k++) {
        char idx[16] = "";
        html_dom_get_attr(items[k], "data-part", idx, sizeof(idx));
        cJSON *part = cJSON_GetArrayItem(objects, atoi(idx));
        if (!part) continue;
        if (html_dom_get_checked(items[k])) ed_json_set_num(part, "soft", 1);
        else cJSON_DeleteItemFromObjectCaseSensitive(part, "soft");
    }
    g_ed.dirty_counts = 1;
    ed_mark_changed();
    ed_clear_pops();
}

static void remove_archetype(int id) {
    if (id < 0) id = id_value_int("archetypeId", 0);
    if (!ed_archetype_get(id)) {
        ed_clear_pops();
        return;
    }
    const int users = entities_using(id);
    if (users) {
        char msg[96];
        snprintf(msg, sizeof(msg), T_ARCHETYPE_IN_USE, users);
        popup(msg);
        return;
    }
    push_history();
    cJSON *list = archetypes(0);
    int i = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, list) {
        if ((int) ed_json_num(a, "id", 0) == id) break;
        i++;
    }
    cJSON_DeleteItemFromArray(list, i);
    g_ed.dirty_counts = 1;
    ed_mark_changed();
    ed_clear_pops();
}

// ---------------------------------------------------------------------------
// archetypeBrowser / duplicateArchetype / removeArchetypeFromBrowser (editor-10.0.7.js:18610-18645)

void ed_archetype_browser(void) {
    if (arch_active()) {
        popup(T_STAGE_ACTIVE);
        return;
    }
    EdBuf rows, b;
    edb_init(&rows);
    edb_init(&b);
    const cJSON *a;
    cJSON_ArrayForEach(a, archetypes(0)) {
        const int id = (int) ed_json_num(a, "id", 0);
        edb_add(&rows, "<div style='display:flex;align-items:center;gap:6px;margin-top:6px;font-size:15px;color:#eee'><span style='flex:1;text-align:left;overflow:hidden;white-space:nowrap'>");
        archetype_label(&rows, a);
        edb_addf(&rows, " <span style='color:#999;font-size:12px'>#%d &middot; %d parts &middot; %d placed</span></span>"
                        "<div class='eButton teleBtn' style='margin:0;padding:4px 8px;font-size:13px;' onclick='KE.enterArchetypeStage(%d)'>" T_EDIT_PARTS "</div>"
                        "<div class='eButton teleBtn' style='margin:0;padding:4px 8px;font-size:13px;' onclick='KE.archetypePopup(%d)'>" T_PROPERTIES "</div>"
                        "<div class='eButton teleBtn' style='margin:0;padding:4px 8px;font-size:13px;' onclick='KE.duplicateArchetype(%d)'>" T_DUPLICATE "</div>"
                        "<div class='eButton teleBtn' style='margin:0;padding:4px 8px;font-size:13px;background-color:#fc5a4e;' onclick='KE.removeArchetypeFromBrowser(%d)'>" T_REMOVE_ARCHETYPE
                        "</div></div>",
                 id, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(a, "objects")), entities_using(id), id, id, id, id);
    }
    edb_add(&b, "<div class='pubHed'>" T_BROWSER_TITLE "</div><div style='max-height:300px;overflow-y:auto;min-width:460px;'>");
    edb_add(&b, rows.len ? rows.buf : "<div style='margin-top:10px;color:#999;font-size:14px'>" T_NO_ARCHETYPES_YET "</div>");
    edb_add(&b, "</div><div class='eButton teleBtn' style='margin-top:14px;' onclick='KE.enterArchetypeStage()'>" T_NEW_ARCHETYPE "</div>");
    ed_generic_popup(b.buf, "entityPop");
    edb_free(&rows);
    edb_free(&b);
}

static void duplicate_archetype(const int id) {
    if (archetype_total() >= limit_of("archetypeLimit", 32)) {
        popup(T_ARCHETYPE_LIMIT);
        return;
    }
    const cJSON *src = ed_archetype_get(id);
    if (!src) return;
    push_history();
    src = ed_archetype_get(id);
    char name[64];
    const char *sn = ed_json_str(src, "name", "");
    if (sn[0]) snprintf(name, sizeof(name), "%s copy", sn);
    else snprintf(name, sizeof(name), "Archetype %d copy", id);
    name[24] = 0; // .substr(0, 24)
    cJSON *a = cJSON_CreateObject();
    cJSON_AddNumberToObject(a, "id", next_archetype_id());
    cJSON_AddStringToObject(a, "name", name);
    cJSON_AddItemToObject(a, "objects", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(src, "objects"), 1));
    cJSON_AddNumberToObject(a, "coll", ed_json_num(src, "coll", 1));
    cJSON_AddNumberToObject(a, "hp", ed_json_num(src, "hp", 0));
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(src, "data");
    cJSON_AddItemToObject(a, "data", cJSON_IsObject(data) ? cJSON_Duplicate(data, 1) : cJSON_CreateObject());
    cJSON_AddItemToArray(archetypes(1), a);
    g_ed.dirty_counts = 1;
    ed_mark_changed();
    ed_archetype_browser();
}

static void remove_from_browser(const int id) {
    remove_archetype(id);
    if (!ed_archetype_get(id)) ed_archetype_browser();
}

// ---------------------------------------------------------------------------
// the archetype stage (editor-10.0.7.js:18646-18770)

static int on_stage(const EditorObject *o) {
    if (o->uuid == g_arch.plate || editor_prefab_has(o->prefab, EP_NOEXPORT) || editor_prefab_has(o->prefab, EP_ISENTITY)) return 0;
    const vec3 p = ed_obj_vec(o, "p", (vec3) {0});
    return fabsf(p.x - k_stage.x) < k_stage.radius && fabsf(p.y - k_stage.y) < k_stage.radius && fabsf(p.z - k_stage.z) < k_stage.radius;
}

int ed_entity_stage_member(const EditorObject *o) { return arch_active() && o && (o->uuid == g_arch.plate || on_stage(o)); }

static size_t stage_objects(EditorObject **out, const size_t max) {
    size_t n = 0;
    for (size_t i = 0; arch_active() && i < g_ed.proj->count; i++) {
        if (on_stage(g_ed.proj->objects[i])) {
            if (out && n < max) out[n] = g_ed.proj->objects[i];
            n++;
        }
    }
    return n;
}

static void enter_stage(const int has_id, const int id) {
    if (arch_active()) {
        popup(T_STAGE_ACTIVE);
        return;
    }
    if (part_active()) {
        popup(T_EDIT_ACTIVE);
        return;
    }
    const cJSON *a = has_id ? ed_archetype_get(id) : NULL;
    if (has_id && !a) return;
    if (!a && archetype_total() >= limit_of("archetypeLimit", 32)) {
        popup(T_ARCHETYPE_LIMIT);
        return;
    }
    ed_clear_pops();
    ed_obj_select(NULL); // hideTransform
    g_arch.proj = proj_id();
    g_arch.active = 1;
    g_arch.ar_id = a ? (int) ed_json_num(a, "id", 0) : 0;
    g_arch.cam = g_ed.proj->camera.position;
    g_arch.yaw = g_ed.proj->yaw;
    g_arch.pitch = g_ed.proj->pitch;
    g_arch.undo_len = g_ed.proj->undo_count;
    g_arch.plate = 0;
    cJSON *d = cJSON_Parse("{\"p\":[0,0,0],\"s\":[280,1,280],\"c\":\"#23202a\"}");
    const double pp[3] = {k_stage.x, k_stage.y - 1, k_stage.z};
    cJSON_ReplaceItemInObjectCaseSensitive(d, "p", cJSON_CreateDoubleArray(pp, 3));
    EditorObject *plate = ed_obj_new(d, 0);
    cJSON_Delete(d);
    if (plate) {
        g_arch.plate = plate->uuid;
        ed_obj_add(plate, 1, 0); // addObject(plate, true, false, true)
    }
    if (a) {
        cJSON *rec = parts_to_records(cJSON_GetObjectItemCaseSensitive(a, "objects"));
        cJSON *r;
        cJSON_ArrayForEach(r, rec) {
            if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "p"))) {
                cJSON_DeleteItemFromObjectCaseSensitive(r, "p");
                cJSON_AddItemToObject(r, "p", cJSON_CreateDoubleArray((const double[3]) {0, 0, 0}, 3));
            }
            const double p[3] = {pnum(r, "p", 0) + k_stage.x, pnum(r, "p", 1) + k_stage.y, pnum(r, "p", 2) + k_stage.z};
            cJSON_ReplaceItemInObjectCaseSensitive(r, "p", cJSON_CreateDoubleArray(p, 3));
            ed_obj_add(ed_obj_new(r, 0), 1, 0);
        }
        cJSON_Delete(rec);
    }
    g_ed.proj->camera.position = (vec3) {k_stage.x + 55, k_stage.y + 55, k_stage.z + 115};
    g_ed.proj->pitch = -0.32f; // controls.setRotation(-0.32, 0.4)
    g_ed.proj->yaw = 0.4f;
    stage_bar_remove();
    const HtmlElem bar = html_dom_create("div", "archStageBar");
    html_dom_set_attr(bar, "style", "position:fixed;top:56px;left:50%;transform:translateX(-50%);z-index:9999;background:rgba(20,18,26,0.92);"
                                    "border:1px solid rgba(255,255,255,0.18);border-radius:8px;padding:10px 16px;color:#fff;text-align:center;");
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "<div style='font-size:16px;margin-bottom:4px'>");
    if (a) {
        edb_add(&b, T_STAGE_EDITING);
        archetype_label(&b, a);
    } else {
        edb_add(&b, T_STAGE_NEW);
    }
    edb_add(&b, "</div><div style='font-size:12px;color:#bbb;margin-bottom:8px'>" T_STAGE_HINT
                "</div><div class='eButton teleBtn' style='display:inline-block;margin:0;' onclick='KE.saveArchetypeStage()'>" T_SAVE
                "</div><div class='eButton teleBtn' style='display:inline-block;margin:0 0 0 10px;background-color:#fc5a4e;' onclick='KE.cancelArchetypeStage()'>" T_CANCEL_PARTS "</div>");
    html_dom_set_html(bar, b.buf);
    edb_free(&b);
    html_dom_append(html_dom_body(), bar);
    g_ed.dirty_counts = 1;
}

static void exit_stage(void) {
    if (!arch_active()) return;
    EditorObject *parts[1024];
    const size_t n = stage_objects(parts, 1024);
    for (size_t i = 0; i < n && i < 1024; i++) ed_obj_remove(parts[i], 0, 0);
    EditorObject *plate = ed_obj_find(g_arch.plate);
    if (plate) ed_obj_remove(plate, 0, 0);
    ed_obj_select(NULL);
    g_ed.proj->camera.position = g_arch.cam;
    g_ed.proj->yaw = g_arch.yaw;
    g_ed.proj->pitch = g_arch.pitch;
    ed_history_truncate(g_arch.undo_len);
    stage_bar_remove();
    g_arch.active = 0;
}

static void save_stage_named(void *user, const char *name) {
    (void) user;
    if (!arch_active()) return;
    EditorObject *parts[1024];
    const size_t n = stage_objects(parts, 1024);
    if (!n) {
        popup(T_NO_STAGE_PARTS);
        return;
    }
    if (n > (size_t) limit_of("archetypeObjectLimit", 40)) {
        popup(T_ARCHETYPE_OBJECT_LIMIT);
        return;
    }
    int id = g_arch.ar_id;
    if (!id && (!name || !*name)) {
        popup(T_NAME_REQUIRED);
        return;
    }
    cJSON *before = ed_archetypes_snapshot();
    cJSON *rec = cJSON_CreateArray();
    for (size_t i = 0; i < n && i < 1024; i++) cJSON_AddItemToArray(rec, ed_obj_serialize(parts[i]));
    apply_center(rec);
    records_to_parts(rec);
    if (!id) {
        id = next_archetype_id();
        cJSON_AddItemToArray(archetypes(1), new_archetype(id, name, rec));
    } else {
        cJSON *a = (cJSON *) ed_archetype_get(id);
        if (a) cJSON_ReplaceItemInObjectCaseSensitive(a, "objects", rec);
        else cJSON_Delete(rec);
    }
    const int named = !g_arch.ar_id;
    exit_stage();
    ed_history_add_archetypes(before);
    g_ed.dirty_counts = 1;
    ed_mark_changed();
    if (named) archetype_popup(id);
    else ed_archetype_browser();
}

static void save_stage(void) {
    if (!arch_active()) return;
    const size_t n = stage_objects(NULL, 0);
    if (!n) {
        popup(T_NO_STAGE_PARTS);
        return;
    }
    if (n > (size_t) limit_of("archetypeObjectLimit", 40)) {
        popup(T_ARCHETYPE_OBJECT_LIMIT);
        return;
    }
    if (!g_arch.ar_id) ed_prompt(T_NAME_PROMPT, "", save_stage_named, NULL);
    else save_stage_named(NULL, NULL);
}

static void cancel_stage(void) {
    exit_stage();
    ed_archetype_browser();
}

// ---------------------------------------------------------------------------
// handlers

static int arg_int(const HtmlArg *a, const int n, const int i, int *out) {
    if (i >= n) return 0;
    if (a[i].kind == HTML_ARG_NUM) *out = (int) a[i].num;
    else if (a[i].kind == HTML_ARG_STR && a[i].str) *out = (int) strtol(a[i].str, NULL, 10);
    else return 0;
    return 1;
}

static int h_entity(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u;
    const char *name = html_ui_current_handler();
    if (!name) return 1;
    int id = 0;
    const int has = arg_int(a, n, 0, &id);
    if (!g_ed.proj) return 1;
    if (!strcmp(name, "KE.createEntity")) create_entity(has ? id : 0);
    else if (!strcmp(name, "KE.saveEntity")) save_entity();
    else if (!strcmp(name, "KE.pickPoint")) pick_point(n >= 1 && a[0].kind == HTML_ARG_STR && a[0].str && !strcmp(a[0].str, "sp"));
    else if (!strcmp(name, "KE.checkEntityJSON")) cJSON_Delete(check_json(ev ? ev->target : 0));
    else if (!strcmp(name, "KE.archetypePopup")) archetype_popup(has ? id : -1);
    else if (!strcmp(name, "KE.saveArchetypeData")) save_archetype_data();
    else if (!strcmp(name, "KE.editEntityParts")) edit_entity_parts();
    else if (!strcmp(name, "KE.applyEntityParts")) apply_entity_parts();
    else if (!strcmp(name, "KE.cancelEntityParts")) cancel_entity_parts();
    else if (!strcmp(name, "KE.unpackArchetype")) unpack_archetype();
    else if (!strcmp(name, "KE.removeArchetype")) remove_archetype(has ? id : -1);
    else if (!strcmp(name, "KE.enterArchetypeStage")) enter_stage(has, id);
    else if (!strcmp(name, "KE.duplicateArchetype")) duplicate_archetype(id);
    else if (!strcmp(name, "KE.removeArchetypeFromBrowser")) remove_from_browser(id);
    else if (!strcmp(name, "KE.saveArchetypeStage")) save_stage();
    else if (!strcmp(name, "KE.cancelArchetypeStage")) cancel_stage();
    else if (!strcmp(name, "KE.saveArchetype")) ed_entity_save_archetype();
    else if (!strcmp(name, "KE.placeEntity")) ed_entity_place();
    else if (!strcmp(name, "KE.entityPopup") || !strcmp(name, "KE.panelEditEntity")) ed_entity_popup();
    else if (!strcmp(name, "KE.panelEditArchetype")) archetype_popup(-1);
    else if (!strcmp(name, "KE.archetypeBrowser")) ed_archetype_browser();
    return 1;
}

void ed_entities_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    static const char *const names[] = {
        "KE.createEntity", "KE.saveEntity", "KE.pickPoint", "KE.checkEntityJSON", "KE.archetypePopup", "KE.saveArchetypeData",
        "KE.editEntityParts", "KE.applyEntityParts", "KE.cancelEntityParts", "KE.unpackArchetype", "KE.removeArchetype",
        "KE.enterArchetypeStage", "KE.duplicateArchetype", "KE.removeArchetypeFromBrowser", "KE.saveArchetypeStage",
        "KE.cancelArchetypeStage", "KE.saveArchetype", "KE.placeEntity", "KE.entityPopup", "KE.panelEditEntity",
        "KE.panelEditArchetype", "KE.archetypeBrowser",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && *count < max; i++) table[(*count)++] = (HtmlHandlerEntry) {names[i], h_entity};
}

// KRUNKNATIVE_EDITOR_SCRIPT "entity:<sub>" (tests): val:<id>=<value> sets a field of the popup,
// check:<id|.class#n>=0|1 a checkbox, prompt:<text> answers the open prompt, pickat:x,y,z ends a
// pickPoint at that world point, state:<file> writes {archEdit, partEdit, pick, bar}
int ed_entities_script(const char *arg) {
    if (!arg) return 0;
    if (!strncmp(arg, "val:", 4) || !strncmp(arg, "check:", 6)) {
        const int check = arg[0] == 'c';
        char id[96];
        const char *eq = strchr(arg, '=');
        const char *from = arg + (check ? 6 : 4);
        if (!eq || (size_t) (eq - from) >= sizeof(id)) return 1;
        snprintf(id, sizeof(id), "%.*s", (int) (eq - from), from);
        HtmlElem e = 0;
        char *hash = strchr(id, '#');
        if (id[0] == '.' && hash) {
            *hash = 0;
            HtmlElem items[64];
            const int n = html_dom_query_all(html_dom_by_id("genericPop"), id, items, 64);
            const int k = atoi(hash + 1);
            if (k >= 0 && k < n) e = items[k];
        } else {
            e = html_dom_by_id(id);
        }
        if (!e) {
            fprintf(stderr, "[entity] no element %s\n", id);
            return 1;
        }
        if (check) html_dom_set_checked(e, atoi(eq + 1));
        else html_dom_set_value(e, eq + 1);
        return 1;
    }
    if (!strncmp(arg, "menu:", 5)) {
        // the toolbar option labelled so (Object > Group > Save as Archetype, Object > Entity > Place, ...);
        // "label#n" = the n-th option of that label (Export#1 = Object > Group > Prefab > Export)
        char label[128];
        snprintf(label, sizeof(label), "%s", arg + 5);
        char *hash = strrchr(label, '#');
        int nth = 0, seen = 0;
        if (hash) {
            *hash = 0;
            nth = atoi(hash + 1);
        }
        HtmlElem items[256];
        const int n = html_dom_query_all(html_dom_by_id("_toolbar"), ".toolbarDropDownItem", items, 256);
        for (int i = 0; i < n && i < 256; i++) {
            char text[128] = "", h[256] = "";
            html_dom_get_text(items[i], text, sizeof(text));
            if (strcmp(text, label) || seen++ != nth || html_dom_get_attr(items[i], "onclick", h, sizeof(h)) <= 0) continue;
            const char *p = strstr(h, "GUI.action(");
            if (!p) continue;
            HtmlEvent ev = {.type = "click", .target = items[i]};
            HtmlArg a[2] = {{HTML_ARG_NUM, atoi(p + 11), NULL}, {HTML_ARG_THIS, 0, NULL}};
            html_ui_dispatch("GUI.action", &ev, a, 2);
            return 1;
        }
        fprintf(stderr, "[entity] no menu option %s\n", arg + 5);
        return 1;
    }
    if (!strncmp(arg, "prompt:", 7)) {
        const HtmlElem in = html_dom_by_id("knPromptInput");
        if (in) html_dom_set_value(in, arg + 7);
        HtmlEvent ev = {.type = "click"};
        html_ui_dispatch("KE.kn.promptOk", &ev, NULL, 0);
        return 1;
    }
    if (!strncmp(arg, "pickat:", 7)) {
        float x = 0, y = 0, z = 0;
        if (sscanf(arg + 7, "%f,%f,%f", &x, &y, &z) == 3 && g_pick.active) pick_done((vec3) {x, y, z});
        return 1;
    }
    if (!strncmp(arg, "popup:", 6)) {
        // the popup: "SHOWN <0|1> <class>", "TEXT <textContent>", then "F <id|class#data-part> <checked> <value>" per field
        FILE *f = fopen(arg + 6, "w");
        if (!f) return 1;
        const HtmlElem pop = html_dom_by_id("genericPop");
        char disp[32] = "", cls[64] = "";
        html_dom_get_style(html_dom_by_id("popupHolder"), "display", disp, sizeof(disp));
        html_dom_get_attr(pop, "class", cls, sizeof(cls));
        fprintf(f, "SHOWN %d %s\n", strcmp(disp, "none") != 0, cls);
        char *text = calloc(1, VALUE_MAX);
        if (text && pop) html_dom_get_text(pop, text, VALUE_MAX);
        for (char *c = text; c && *c; c++) if (*c == '\n') *c = ' ';
        fprintf(f, "TEXT %s\n", text ? text : "");
        free(text);
        char msg[256] = "";
        const HtmlElem prompt = html_dom_query(html_dom_by_id("knPrompt"), ".knPromptMsg");
        if (prompt) html_dom_get_text(prompt, msg, sizeof(msg));
        fprintf(f, "PROMPT %s\n", msg);
        HtmlElem items[256];
        const int n = pop ? html_dom_query_all(pop, "input, select, textarea", items, 256) : 0;
        for (int i = 0; i < n && i < 256; i++) {
            char id[64] = "", part[16] = "";
            if (html_dom_get_attr(items[i], "id", id, sizeof(id)) <= 0) {
                html_dom_get_attr(items[i], "class", id, sizeof(id));
                const char *last = strrchr(id, ' ');
                memmove(id, last ? last + 1 : id, strlen(last ? last + 1 : id) + 1);
                html_dom_get_attr(items[i], "data-part", part, sizeof(part));
            }
            char *v = value_of(items[i]);
            for (char *c = v; c && *c; c++) if (*c == '\n') *c = ' ';
            fprintf(f, "F %s%s%s %d %s\n", id, part[0] ? "#" : "", part, html_dom_get_checked(items[i]), v ? v : "");
            free(v);
        }
        fclose(f);
        return 1;
    }
    if (!strncmp(arg, "state:", 6)) {
        FILE *f = fopen(arg + 6, "w");
        if (!f) return 1;
        fprintf(f, "archEdit=%d arId=%d partEdit=%d partArId=%d pick=%d bar=%d undos=%zu archetypes=%d\n", arch_active(), g_arch.ar_id, part_active(),
                g_part.ar_id, g_pick.active, html_dom_by_id("archStageBar") != 0, g_ed.proj->undo_count, archetype_total());
        fclose(f);
        return 1;
    }
    return 0;
}
