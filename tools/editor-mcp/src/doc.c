#define _GNU_SOURCE
#include "doc.h"
#include "jsonw.h"
#include "schema.h"
#include "util.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HISTORY_MAX_ENTRIES 200
#define HISTORY_MAX_BYTES ((size_t) 256 << 20)

// ---------------------------------------------------------------- lifecycle

void doc_assign_missing_ids(Doc *doc) {
    cJSON *objects = doc_objects(doc);
    for (cJSON *o = objects->child; o; o = o->next) {
        if (o->valueint > doc->next_uid - 1) doc->next_uid = o->valueint + 1;
    }
    for (cJSON *o = objects->child; o; o = o->next) {
        if (o->valueint <= 0 && cJSON_IsObject(o)) o->valueint = doc->next_uid++;
    }
}

static Doc *doc_wrap(cJSON *root) {
    Doc *doc = calloc(1, sizeof(Doc));
    if (!doc) {
        cJSON_Delete(root);
        return NULL;
    }
    doc->root = root;
    doc->next_uid = 1;
    doc_assign_missing_ids(doc);
    return doc;
}

Doc *doc_from_text(const char *text, const size_t len, char *err, const size_t errsz) {
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, 0);
    if (!root) {
        const size_t at = end && end >= text ? (size_t) (end - text) : 0;
        snprintf(err, errsz, "invalid JSON near byte %zu", at);
        return NULL;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, errsz, "a Krunker map is a JSON object");
        return NULL;
    }
    const cJSON *objects = js_get(root, "objects");
    if (objects && !cJSON_IsArray(objects)) {
        cJSON_Delete(root);
        snprintf(err, errsz, "\"objects\" is not an array");
        return NULL;
    }
    Doc *doc = doc_wrap(root);
    if (!doc) return NULL;
    for (size_t i = 0; i + 1 < len && i < 4096; i++) {
        if (text[i] == '\n' && (text[i + 1] == ' ' || text[i + 1] == '\t' || text[i + 1] == '"')) {
            doc->source_pretty = 1;
            break;
        }
    }
    doc->trailing_newline = len > 0 && text[len - 1] == '\n';
    return doc;
}

static cJSON *make_cube(const double x, const double y, const double z, const double sx, const double sy, const double sz, const int texture) {
    cJSON *o = cJSON_CreateObject();
    const double p[3] = {x, y, z}, s[3] = {sx, sy, sz};
    cJSON_AddItemToObject(o, "p", js_vec3_new(p));
    cJSON_AddItemToObject(o, "s", js_vec3_new(s));
    if (texture) cJSON_AddNumberToObject(o, "t", texture);
    return o;
}

static void add_spawn(cJSON *spawns, const double x, const double y, const double z, const int team, const int dir) {
    cJSON *s = cJSON_CreateArray();
    const double v[6] = {x, y, z, team, dir, 0};
    for (int i = 0; i < 6; i++) cJSON_AddItemToArray(s, cJSON_CreateNumber(v[i]));
    cJSON_AddItemToArray(spawns, s);
}

Doc *doc_from_template(const char *name, const char *map_name) {
    if (!name || !name[0]) name = "blank";
    const int empty = !strcmp(name, "empty"), blank = !strcmp(name, "blank"), arena = !strcmp(name, "arena");
    if (!empty && !blank && !arena) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", map_name && map_name[0] ? map_name : "New Krunker Map");
    cJSON_AddStringToObject(root, "ambient", "#97a0a8");
    cJSON_AddStringToObject(root, "light", "#f2f8fc");
    cJSON_AddStringToObject(root, "sky", "#dce8ed");
    cJSON_AddStringToObject(root, "fog", "#8d9aa0");
    cJSON_AddNumberToObject(root, "fogD", 2000);
    cJSON *objects = cJSON_AddArrayToObject(root, "objects");
    const int grid = schema_texture_find("GRID") > 0 ? schema_texture_find("GRID") : 3;
    const int wall = schema_texture_find("WALL") >= 0 ? schema_texture_find("WALL") : 0;
    if (blank) cJSON_AddItemToArray(objects, make_cube(0, -10, 0, 200, 10, 200, grid));
    if (arena) {
        cJSON_AddItemToArray(objects, make_cube(0, -10, 0, 400, 10, 400, grid));
        cJSON_AddItemToArray(objects, make_cube(0, 0, -205, 420, 40, 10, wall));
        cJSON_AddItemToArray(objects, make_cube(0, 0, 205, 420, 40, 10, wall));
        cJSON_AddItemToArray(objects, make_cube(-205, 0, 0, 10, 40, 400, wall));
        cJSON_AddItemToArray(objects, make_cube(205, 0, 0, 10, 40, 400, wall));
        cJSON_AddItemToArray(objects, make_cube(0, 0, 0, 40, 12, 40, wall));
    }
    cJSON *spawns = cJSON_AddArrayToObject(root, "spawns");
    if (blank) {
        add_spawn(spawns, -40, 0, 0, 0, 1);
        add_spawn(spawns, 40, 0, 0, 0, 3);
    }
    if (arena) {
        for (int i = 0; i < 4; i++) add_spawn(spawns, -160, 0, -90 + 60 * i, 1, 1);
        for (int i = 0; i < 4; i++) add_spawn(spawns, 160, 0, -90 + 60 * i, 2, 3);
        cJSON *config = cJSON_AddObjectToObject(root, "config");
        cJSON *modes = cJSON_AddArrayToObject(config, "modes");
        cJSON_AddItemToArray(modes, cJSON_CreateNumber(0));
        cJSON_AddItemToArray(modes, cJSON_CreateNumber(1));
    }
    Doc *doc = doc_wrap(root);
    if (doc) doc->dirty = 1;
    return doc;
}

static void hist_free(DocHist *h) {
    for (int i = 0; i < h->count; i++) {
        free(h->items[i].text);
        free(h->items[i].uids);
    }
    free(h->items);
    memset(h, 0, sizeof(*h));
}

void doc_history_clear(Doc *doc) {
    hist_free(&doc->undo);
    hist_free(&doc->redo);
    doc->history_bytes = 0;
}

void doc_free(Doc *doc) {
    if (!doc) return;
    doc_history_clear(doc);
    cJSON_Delete(doc->root);
    free(doc);
}

// ---------------------------------------------------------------- objects

cJSON *doc_objects(Doc *doc) {
    cJSON *o = cJSON_GetObjectItemCaseSensitive(doc->root, "objects");
    if (!cJSON_IsArray(o)) {
        cJSON_DeleteItemFromObjectCaseSensitive(doc->root, "objects");
        o = cJSON_AddArrayToObject(doc->root, "objects");
    }
    return o;
}

int doc_object_count(Doc *doc) { return cJSON_GetArraySize(doc_objects(doc)); }
cJSON *doc_object_at(Doc *doc, const int index) { return index < 0 ? NULL : cJSON_GetArrayItem(doc_objects(doc), index); }
int doc_object_id(const cJSON *obj) { return obj ? obj->valueint : 0; }

cJSON *doc_object_by_id(Doc *doc, const int id, int *index_out) {
    int i = 0;
    for (cJSON *o = doc_objects(doc)->child; o; o = o->next, i++) {
        if (o->valueint == id) {
            if (index_out) *index_out = i;
            return o;
        }
    }
    return NULL;
}

int doc_add_object(Doc *doc, cJSON *obj, const int index) {
    cJSON *objects = doc_objects(doc);
    if (obj->valueint == 0) obj->valueint = doc->next_uid++;
    else if (obj->valueint >= doc->next_uid) doc->next_uid = obj->valueint + 1;
    if (index < 0 || index >= cJSON_GetArraySize(objects)) cJSON_AddItemToArray(objects, obj);
    else cJSON_InsertItemInArray(objects, index, obj);
    return obj->valueint;
}

int doc_remove_object(Doc *doc, const int id) {
    int index;
    if (!doc_object_by_id(doc, id, &index)) return -1;
    cJSON_DeleteItemFromArray(doc_objects(doc), index);
    return 0;
}

// ---------------------------------------------------------------- semantics

int obj_prefab_raw_valid(const cJSON *obj) {
    const cJSON *i = js_get(obj, "i");
    const cJSON *id = js_get(obj, "id");
    const cJSON *v = i && !cJSON_IsNull(i) ? i : id;
    if (!v || cJSON_IsNull(v)) return 1; // CUBE
    return schema_prefab_from_json(v) >= 0;
}

int obj_prefab(const cJSON *obj) {
    const cJSON *i = js_get(obj, "i");
    const cJSON *id = js_get(obj, "id");
    // game.js customMap: prefabs[i || id || 0]
    const cJSON *v = js_truthy(i) ? i : js_truthy(id) ? id : NULL;
    const int p = v ? schema_prefab_from_json(v) : 0;
    return p < 0 ? 0 : p;
}

int obj_pos(const cJSON *obj, double out[3]) {
    if (js_vec3(js_get(obj, "p"), out)) return 1;
    out[0] = out[1] = out[2] = 0;
    return 0;
}

void obj_size(const Doc *doc, const cJSON *obj, double out[3]) {
    if (js_vec3(js_get(obj, "s"), out)) return;
    const cJSON *si = js_get(obj, "si");
    const cJSON *xyz = doc ? js_get(doc->root, "xyz") : NULL;
    if (cJSON_IsNumber(si) && cJSON_IsArray(xyz)) {
        const int base = (int) si->valuedouble * 3;
        const cJSON *a = cJSON_GetArrayItem(xyz, base), *b = cJSON_GetArrayItem(xyz, base + 1), *c = cJSON_GetArrayItem(xyz, base + 2);
        if (cJSON_IsNumber(a) && cJSON_IsNumber(b) && cJSON_IsNumber(c)) {
            out[0] = a->valuedouble;
            out[1] = b->valuedouble;
            out[2] = c->valuedouble;
            return;
        }
    }
    const SchemaPrefab *p = schema_prefab(obj_prefab(obj));
    for (int k = 0; k < 3; k++) out[k] = p ? p->default_size[k] : 10;
}

void obj_rot(const cJSON *obj, double out[3]) {
    if (!js_vec3(js_get(obj, "r"), out)) out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < 3; k++) if (!isfinite(out[k])) out[k] = 0;
}

void obj_aabb(const Doc *doc, const cJSON *obj, double mn[3], double mx[3]) {
    double p[3], s[3];
    obj_pos(obj, p);
    obj_size(doc, obj, s);
    mn[0] = p[0] - s[0] / 2;
    mx[0] = p[0] + s[0] / 2;
    mn[1] = p[1];
    mx[1] = p[1] + s[1];
    mn[2] = p[2] - s[2] / 2;
    mx[2] = p[2] + s[2] / 2;
}

void obj_bounds(const Doc *doc, const cJSON *obj, double mn[3], double mx[3]) {
    double p[3], s[3], r[3];
    obj_pos(obj, p);
    obj_size(doc, obj, s);
    obj_rot(obj, r);
    if (!r[0] && !r[1] && !r[2]) {
        obj_aabb(doc, obj, mn, mx);
        return;
    }
    // three.js Euler XYZ: M = Rx * Ry * Rz
    const double cx = cos(r[0]), sx = sin(r[0]), cy = cos(r[1]), sy = sin(r[1]), cz = cos(r[2]), sz = sin(r[2]);
    const double m[3][3] = {
        {cy * cz, -cy * sz, sy},
        {cx * sz + sx * sy * cz, cx * cz - sx * sy * sz, -sx * cy},
        {sx * sz - cx * sy * cz, sx * cz + cx * sy * sz, cx * cy},
    };
    const double c[3] = {p[0], p[1] + s[1] / 2, p[2]};
    for (int k = 0; k < 3; k++) {
        // half extent of the rotated box along world axis k
        const double e = fabs(m[k][0]) * s[0] / 2 + fabs(m[k][1]) * s[1] / 2 + fabs(m[k][2]) * s[2] / 2;
        mn[k] = c[k] - e;
        mx[k] = c[k] + e;
    }
}

int color_parse(const cJSON *v, char out[8]) {
    if (cJSON_IsNumber(v)) {
        snprintf(out, 8, "#%06x", (unsigned) ((long long) v->valuedouble & 0xffffff));
        return 1;
    }
    if (!cJSON_IsString(v)) return 0;
    const char *s = v->valuestring;
    if (*s == '#') s++;
    else if (!strncasecmp(s, "0x", 2)) s += 2;
    const size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char) s[i])) return 0;
    if (n == 3) {
        snprintf(out, 8, "#%c%c%c%c%c%c", s[0], s[0], s[1], s[1], s[2], s[2]);
    } else if (n == 6) {
        snprintf(out, 8, "#%s", s);
    } else {
        return 0;
    }
    for (int i = 1; i < 7; i++) out[i] = (char) tolower((unsigned char) out[i]);
    return 1;
}

int obj_color(const Doc *doc, const cJSON *obj, const char *key, char out[8]) {
    const int emissive = key[0] == 'e';
    snprintf(out, 8, "%s", emissive ? "#000000" : "#ffffff");
    const cJSON *v = js_get(obj, key);
    if (v && js_truthy(v) && color_parse(v, out)) return 1;
    char ikey[8];
    snprintf(ikey, sizeof(ikey), "%si", key);
    const cJSON *idx = js_get(obj, ikey);
    const cJSON *palette = doc ? js_get(doc->root, "colors") : NULL;
    if (cJSON_IsNumber(idx) && cJSON_IsArray(palette)) {
        const cJSON *c = cJSON_GetArrayItem(palette, (int) idx->valuedouble);
        if (c && color_parse(c, out)) return 1;
    }
    return 0;
}

int obj_texture(const cJSON *obj) {
    const int prefab = obj_prefab(obj);
    const cJSON *t = js_get(obj, "t");
    if (cJSON_IsString(t)) {
        if (!t->valuestring[0]) return schema_texture_default(prefab);
        const int i = schema_texture_find(t->valuestring);
        return i >= 0 ? i : 0;
    }
    if (cJSON_IsNumber(t) && t->valuedouble != 0) return (int) t->valuedouble;
    return schema_texture_default(prefab);
}

int obj_collidable(const cJSON *obj) {
    const int prefab = obj_prefab(obj);
    if (schema_prefab_has_flag(prefab, "forceCollision")) return 1;
    if (schema_prefab_has_flag(prefab, "noCollision") || schema_prefab_has_flag(prefab, "noCol")) return 0;
    return !js_truthy(js_get(obj, "l")) && !js_truthy(js_get(obj, "col"));
}

int obj_visible(const cJSON *obj) { return !js_truthy(js_get(obj, "v")); }

static void set_item(cJSON *obj, const char *key, cJSON *value) {
    if (cJSON_GetObjectItemCaseSensitive(obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, value);
    else cJSON_AddItemToObject(obj, key, value);
}

void obj_set_vec(cJSON *obj, const char *key, const double v[3]) {
    double r[3];
    for (int k = 0; k < 3; k++) r[k] = js_round(v[k], 4);
    if (!strcmp(key, "r") && !r[0] && !r[1] && !r[2]) {
        cJSON_DeleteItemFromObjectCaseSensitive(obj, "r");
        return;
    }
    set_item(obj, key, js_vec3_new(r));
    if (!strcmp(key, "s")) cJSON_DeleteItemFromObjectCaseSensitive(obj, "si");
}

// ---------------------------------------------------------------- properties

static void del_keys(cJSON *obj, const SchemaProp *p) {
    cJSON_DeleteItemFromObjectCaseSensitive(obj, p->key);
    const cJSON *a;
    cJSON_ArrayForEach(a, p->alt_keys) if (cJSON_IsString(a)) cJSON_DeleteItemFromObjectCaseSensitive(obj, a->valuestring);
    if (p->palette_key) cJSON_DeleteItemFromObjectCaseSensitive(obj, p->palette_key);
}

int doc_unset_prop(cJSON *obj, const char *name) {
    const SchemaProp *p = schema_prop_lookup(name);
    if (!p) {
        const int had = cJSON_GetObjectItemCaseSensitive(obj, name) != NULL;
        cJSON_DeleteItemFromObjectCaseSensitive(obj, name);
        return had;
    }
    const int had = js_get(obj, p->key) || (p->palette_key && js_get(obj, p->palette_key));
    del_keys(obj, p);
    return had;
}

static int num_value(const cJSON *v, double *out) {
    if (cJSON_IsNumber(v)) *out = v->valuedouble;
    else if (cJSON_IsBool(v)) *out = cJSON_IsTrue(v);
    else if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end;
        *out = strtod(v->valuestring, &end);
        if (*end) return 0;
    } else return 0;
    return isfinite(*out);
}

static int palette_index(Doc *doc, const char *hex) {
    cJSON *palette = cJSON_GetObjectItemCaseSensitive(doc->root, "colors");
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, palette) {
        char c[8];
        if (color_parse(it, c) && !strcmp(c, hex)) return i;
        i++;
    }
    cJSON_AddItemToArray(palette, cJSON_CreateString(hex));
    return i;
}

static int faces_bits(const cJSON *v, int *bits) {
    if (cJSON_IsString(v)) {
        char *end;
        const long b = strtol(v->valuestring, &end, 16);
        if (*end || b < 0 || b > 63) return 0;
        *bits = (int) b;
        return 1;
    }
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 6) {
        int b = 0;
        for (int i = 0; i < 6; i++) if (js_truthy(cJSON_GetArrayItem(v, i))) b |= 1 << (5 - i);
        *bits = b;
        return 1;
    }
    if (cJSON_IsObject(v)) {
        static const char *names[6] = {"right", "left", "top", "bottom", "back", "front"};
        int b = 63;
        for (int i = 0; i < 6; i++) {
            const cJSON *f = js_get(v, names[i]);
            if (f && !js_truthy(f)) b &= ~(1 << (5 - i));
        }
        *bits = b;
        return 1;
    }
    return 0;
}

int doc_set_prop(Doc *doc, cJSON *obj, const char *name, const cJSON *value, const int flags, char *err, const size_t errsz) {
    if ((flags & SET_RAW_UNKNOWN) && (flags & SET_FORCE)) {
        // raw + force: the key exactly as given, no validation (seeding / newer editor fields)
        if (!value || cJSON_IsNull(value)) cJSON_DeleteItemFromObjectCaseSensitive(obj, name);
        else set_item(obj, name, cJSON_Duplicate(value, 1));
        return 0;
    }
    const int prefab = obj_prefab(obj);
    const SchemaPrefab *pf = schema_prefab(prefab);
    if (!strcmp(name, "prefab") || !strcmp(name, "i")) {
        const int id = schema_prefab_from_json(value);
        if (id < 0 && (flags & SET_FORCE) && !strcmp(name, "i") && (cJSON_IsNumber(value) || cJSON_IsString(value))) {
            // force: a raw prefab reference the schema does not know (newer editors)
            cJSON_DeleteItemFromObjectCaseSensitive(obj, "id");
            set_item(obj, "i", cJSON_Duplicate(value, 1));
            return 0;
        }
        if (id < 0) {
            snprintf(err, errsz, "unknown prefab %s", cJSON_IsString(value) ? value->valuestring : "(number)");
            return -1;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(obj, "id");
        if (id == 0) cJSON_DeleteItemFromObjectCaseSensitive(obj, "i");
        else set_item(obj, "i", cJSON_CreateNumber(id));
        return 0;
    }
    const SchemaProp *p = schema_prop_lookup(name);
    if (!p) {
        if (!(flags & (SET_FORCE | SET_RAW_UNKNOWN))) {
            snprintf(err, errsz, "unknown property '%s' (krunker://prefabs lists them; pass force:true to write a raw key)", name);
            return -1;
        }
        if (!value || cJSON_IsNull(value)) cJSON_DeleteItemFromObjectCaseSensitive(obj, name);
        else set_item(obj, name, cJSON_Duplicate(value, 1));
        return 0;
    }
    if (!schema_core_key(p->key) && !schema_prop_allowed(p, prefab) && !(flags & SET_FORCE)) {
        snprintf(err, errsz, "property '%s' (%s) is not available on %s (pass force:true to write it anyway)", p->prop, p->key,
                 pf ? pf->name : "?");
        return -1;
    }
    if (!value || cJSON_IsNull(value)) {
        del_keys(obj, p);
        return 0;
    }
    const double range = schema_limit("positionRange", 500000);
    switch (p->type) {
        case PT_VEC3: {
            double v[3];
            if (!js_vec3(value, v) || !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) {
                snprintf(err, errsz, "'%s' needs [x, y, z] finite numbers", p->prop);
                return -1;
            }
            for (int k = 0; k < 3; k++) {
                if (!strcmp(p->key, "s") && v[k] <= 0) {
                    if (flags & SET_CLAMP) v[k] = 0.01;
                    else {
                        snprintf(err, errsz, "size components must be > 0 (got %g)", v[k]);
                        return -1;
                    }
                }
                if (strcmp(p->key, "r") && fabs(v[k]) > range) {
                    if (flags & SET_CLAMP) v[k] = v[k] < 0 ? -range : range;
                    else {
                        snprintf(err, errsz, "'%s' component %g is outside +-%g (positionRange)", p->prop, v[k], range);
                        return -1;
                    }
                }
            }
            obj_set_vec(obj, p->key, v);
            return 0;
        }
        case PT_NUM:
        case PT_INT: {
            double d;
            if (!num_value(value, &d)) {
                snprintf(err, errsz, "'%s' needs a number", p->prop);
                return -1;
            }
            if (p->type == PT_INT && d != floor(d)) {
                if (flags & SET_CLAMP) d = round(d);
                else {
                    snprintf(err, errsz, "'%s' needs an integer (got %g)", p->prop, d);
                    return -1;
                }
            }
            if ((p->has_min && d < p->min) || (p->has_max && d > p->max)) {
                if (flags & SET_CLAMP) d = p->has_min && d < p->min ? p->min : p->max;
                else {
                    snprintf(err, errsz, "'%s' = %g is outside [%g, %g]", p->prop, d, p->has_min ? p->min : -INFINITY,
                             p->has_max ? p->max : INFINITY);
                    return -1;
                }
            }
            if (p->decimals >= 0) d = js_round(d, p->decimals);
            del_keys(obj, p);
            const int is_default = cJSON_IsNumber(p->def) && p->def->valuedouble == d && strcmp(p->key, "tm");
            if (!is_default) cJSON_AddNumberToObject(obj, p->key, d);
            return 0;
        }
        case PT_BOOL_PRESENT:
            del_keys(obj, p);
            if (js_truthy(value)) cJSON_AddNumberToObject(obj, p->key, 1);
            return 0;
        case PT_BOOL_ABSENT:
            del_keys(obj, p);
            if (!js_truthy(value)) cJSON_AddNumberToObject(obj, p->key, 1);
            return 0;
        case PT_STR: {
            if (!cJSON_IsString(value)) {
                snprintf(err, errsz, "'%s' needs a string", p->prop);
                return -1;
            }
            if (p->max_length >= 0 && (int) strlen(value->valuestring) > p->max_length) {
                snprintf(err, errsz, "'%s' is longer than %d characters", p->prop, p->max_length);
                return -1;
            }
            del_keys(obj, p);
            if (!(cJSON_IsString(p->def) && !strcmp(p->def->valuestring, value->valuestring)))
                cJSON_AddStringToObject(obj, p->key, value->valuestring);
            return 0;
        }
        case PT_COLOR: {
            char hex[8], def[8] = "";
            if (!color_parse(value, hex)) {
                snprintf(err, errsz, "'%s' needs a colour \"#rrggbb\"", p->prop);
                return -1;
            }
            if (p->def) color_parse(p->def, def);
            del_keys(obj, p);
            if (!strcmp(hex, def)) return 0;
            if (p->palette_key && cJSON_IsArray(js_get(doc->root, "colors"))) {
                cJSON_AddNumberToObject(obj, p->palette_key, palette_index(doc, hex));
            } else {
                cJSON_AddStringToObject(obj, p->key, hex);
            }
            return 0;
        }
        case PT_TEXTURE: {
            int t = -1;
            if (cJSON_IsNumber(value)) t = (int) value->valuedouble;
            else if (cJSON_IsString(value)) t = schema_texture_find(value->valuestring);
            if (t < 0 || t >= schema_texture_count()) {
                snprintf(err, errsz, "unknown texture (krunker://textures lists %d)", schema_texture_count());
                return -1;
            }
            del_keys(obj, p);
            if (t != schema_texture_default(prefab)) cJSON_AddNumberToObject(obj, p->key, t);
            return 0;
        }
        case PT_FACES: {
            int bits;
            if (!faces_bits(value, &bits)) {
                snprintf(err, errsz, "'faces' needs 6 booleans [right, left, top, bottom, back, front] or a hex string");
                return -1;
            }
            del_keys(obj, p);
            if (bits != 63) {
                char hex[8];
                snprintf(hex, sizeof(hex), "%x", bits);
                cJSON_AddStringToObject(obj, p->key, hex);
            }
            return 0;
        }
        case PT_ANY:
        default:
            set_item(obj, p->key, cJSON_Duplicate(value, 1));
            return 0;
    }
}

static cJSON *prop_value(const Doc *doc, const cJSON *obj, const SchemaProp *p, int *present) {
    const cJSON *v = js_get(obj, p->key);
    if (!v) {
        const cJSON *a;
        cJSON_ArrayForEach(a, p->alt_keys) if (cJSON_IsString(a) && (v = js_get(obj, a->valuestring))) break;
    }
    *present = v != NULL || (p->palette_key && js_get(obj, p->palette_key));
    switch (p->type) {
        case PT_BOOL_PRESENT: return cJSON_CreateBool(v && !cJSON_IsNull(v));
        case PT_BOOL_ABSENT: return cJSON_CreateBool(!(v && !cJSON_IsNull(v)));
        case PT_COLOR: {
            char hex[8];
            obj_color(doc, obj, p->key, hex);
            if (!*present && p->def) color_parse(p->def, hex);
            return cJSON_CreateString(hex);
        }
        case PT_TEXTURE: {
            const char *n = schema_texture_name(obj_texture(obj));
            return cJSON_CreateString(n ? n : "?");
        }
        case PT_FACES: {
            int bits = 63;
            if (v) faces_bits(v, &bits);
            cJSON *a = cJSON_CreateArray();
            for (int i = 0; i < 6; i++) cJSON_AddItemToArray(a, cJSON_CreateBool(bits >> (5 - i) & 1));
            return a;
        }
        case PT_VEC3: {
            if (!strcmp(p->key, "s")) {
                double s[3];
                obj_size(doc, obj, s);
                return js_vec3_new(s);
            }
            if (v) return cJSON_Duplicate(v, 1);
            return p->def ? cJSON_Duplicate(p->def, 1) : cJSON_CreateNull();
        }
        default:
            if (v) return cJSON_Duplicate(v, 1);
            return p->def ? cJSON_Duplicate(p->def, 1) : cJSON_CreateNull();
    }
}

cJSON *doc_props_json(const Doc *doc, const cJSON *obj, const int with_defaults) {
    cJSON *out = cJSON_CreateObject();
    const int prefab = obj_prefab(obj);
    for (int i = 0; i < schema_prop_count(); i++) {
        const SchemaProp *p = schema_prop_at(i);
        if (schema_core_key(p->key)) continue;
        int present;
        cJSON *v = prop_value(doc, obj, p, &present);
        if (present || (with_defaults && schema_prop_allowed(p, prefab) && !cJSON_IsNull(v))) cJSON_AddItemToObject(out, p->prop, v);
        else cJSON_Delete(v);
    }
    return out;
}

cJSON *doc_object_json(const Doc *doc, const cJSON *obj, const int index, const int detail) {
    cJSON *o = cJSON_CreateObject();
    const int prefab = obj_prefab(obj);
    const SchemaPrefab *pf = schema_prefab(prefab);
    cJSON_AddNumberToObject(o, "id", obj->valueint);
    if (index >= 0) cJSON_AddNumberToObject(o, "index", index);
    cJSON_AddStringToObject(o, "prefab", pf ? pf->name : "?");
    double p[3], s[3], r[3];
    obj_pos(obj, p);
    obj_size(doc, obj, s);
    obj_rot(obj, r);
    cJSON_AddItemToObject(o, "p", js_vec3_new(p));
    cJSON_AddItemToObject(o, "s", js_vec3_new(s));
    if (r[0] || r[1] || r[2]) cJSON_AddItemToObject(o, "r", js_vec3_new(r));
    char hex[8];
    if (obj_color(doc, obj, "c", hex)) cJSON_AddStringToObject(o, "color", hex);
    if (pf && schema_prefab_has_flag(prefab, "texturable")) {
        const char *t = schema_texture_name(obj_texture(obj));
        if (t) cJSON_AddStringToObject(o, "texture", t);
    }
    if (!obj_collidable(obj)) cJSON_AddFalseToObject(o, "collidable");
    if (!obj_visible(obj)) cJSON_AddFalseToObject(o, "visible");
    if (detail <= 0) return o;
    cJSON_AddNumberToObject(o, "prefab_id", prefab);
    if (pf) {
        cJSON_AddStringToObject(o, "label", pf->label);
        cJSON_AddStringToObject(o, "category", pf->category);
    }
    if (!obj_prefab_raw_valid(obj)) cJSON_AddTrueToObject(o, "unknown_prefab");
    double mn[3], mx[3];
    obj_bounds(doc, obj, mn, mx);
    cJSON *b = cJSON_AddObjectToObject(o, "bounds");
    cJSON_AddItemToObject(b, "min", js_vec3_new(mn));
    cJSON_AddItemToObject(b, "max", js_vec3_new(mx));
    cJSON_AddItemToObject(o, "props", doc_props_json(doc, obj, detail >= 2));
    cJSON *unknown = cJSON_CreateArray();
    for (const cJSON *k = obj->child; k; k = k->next) {
        if (schema_core_key(k->string) || schema_prop_by_key(k->string)) continue;
        if (!strcmp(k->string, "ci") || !strcmp(k->string, "ei")) continue;
        cJSON_AddItemToArray(unknown, cJSON_CreateString(k->string));
    }
    if (cJSON_GetArraySize(unknown)) cJSON_AddItemToObject(o, "unknown_keys", unknown);
    else cJSON_Delete(unknown);
    cJSON *raw = cJSON_Duplicate(obj, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(raw, "objUUID");
    cJSON_AddItemToObject(o, "raw", raw);
    return o;
}

// ---------------------------------------------------------------- history

char *doc_snapshot(Doc *doc, int **uids, int *count) {
    char *text = jsonw_print(doc->root, JSONW_COMPACT);
    const int n = doc_object_count(doc);
    int *ids = malloc(sizeof(int) * (size_t) (n ? n : 1));
    int i = 0;
    for (cJSON *o = doc_objects(doc)->child; o && ids; o = o->next) ids[i++] = o->valueint;
    *uids = ids;
    *count = n;
    return text;
}

int doc_restore(Doc *doc, const char *text, const int *uids, const int count, const int next_uid) {
    cJSON *root = cJSON_Parse(text);
    if (!root) return -1;
    cJSON_Delete(doc->root);
    doc->root = root;
    int i = 0;
    for (cJSON *o = doc_objects(doc)->child; o; o = o->next, i++) o->valueint = i < count ? uids[i] : 0;
    if (next_uid > doc->next_uid) doc->next_uid = next_uid;
    doc_assign_missing_ids(doc);
    doc->revision++;
    return 0;
}

static void hist_push(DocHist *h, DocHistEntry e) {
    if (h->count == h->cap) {
        const int cap = h->cap ? h->cap * 2 : 16;
        DocHistEntry *items = realloc(h->items, sizeof(DocHistEntry) * (size_t) cap);
        if (!items) {
            free(e.text);
            free(e.uids);
            return;
        }
        h->items = items;
        h->cap = cap;
    }
    h->items[h->count++] = e;
}

static size_t entry_bytes(const DocHistEntry *e) {
    return (e->text ? strlen(e->text) : 0) + sizeof(int) * (size_t) e->uid_count;
}

static void hist_trim(Doc *doc) {
    while (doc->undo.count > 1 && (doc->undo.count > HISTORY_MAX_ENTRIES || doc->history_bytes > HISTORY_MAX_BYTES)) {
        DocHistEntry *e = &doc->undo.items[0];
        doc->history_bytes -= entry_bytes(e);
        free(e->text);
        free(e->uids);
        memmove(doc->undo.items, doc->undo.items + 1, sizeof(DocHistEntry) * (size_t) (doc->undo.count - 1));
        doc->undo.count--;
    }
}

void doc_history_push(Doc *doc, const char *label, char *text, int *uids, const int count, const int next_uid) {
    for (int i = 0; i < doc->redo.count; i++) {
        doc->history_bytes -= entry_bytes(&doc->redo.items[i]);
        free(doc->redo.items[i].text);
        free(doc->redo.items[i].uids);
    }
    doc->redo.count = 0;
    DocHistEntry e = {.text = text, .uids = uids, .uid_count = count, .time = wall_seconds(), .next_uid = next_uid};
    snprintf(e.label, sizeof(e.label), "%s", label ? label : "edit");
    doc->history_bytes += entry_bytes(&e);
    hist_push(&doc->undo, e);
    hist_trim(doc);
    doc->dirty = 1;
    doc->revision++;
}

static int hist_step(Doc *doc, DocHist *from, DocHist *to, char *label, const size_t n) {
    if (!from->count) return 0;
    DocHistEntry e = from->items[--from->count];
    int *uids;
    int count;
    char *now = doc_snapshot(doc, &uids, &count);
    if (doc_restore(doc, e.text, e.uids, e.uid_count, e.next_uid) != 0) {
        from->count++;
        free(now);
        free(uids);
        return 0;
    }
    if (label) snprintf(label, n, "%s", e.label);
    doc->history_bytes -= entry_bytes(&e);
    free(e.text);
    free(e.uids);
    DocHistEntry back = {.text = now, .uids = uids, .uid_count = count, .time = wall_seconds(), .next_uid = doc->next_uid};
    snprintf(back.label, sizeof(back.label), "%s", label ? label : "edit");
    doc->history_bytes += entry_bytes(&back);
    hist_push(to, back);
    doc->dirty = 1;
    return 1;
}

int doc_undo(Doc *doc, char *label, const size_t n) { return hist_step(doc, &doc->undo, &doc->redo, label, n); }
int doc_redo(Doc *doc, char *label, const size_t n) { return hist_step(doc, &doc->redo, &doc->undo, label, n); }

cJSON *doc_history_json(const Doc *doc, const int limit) {
    cJSON *o = cJSON_CreateObject();
    cJSON *undo = cJSON_AddArrayToObject(o, "undo");
    cJSON *redo = cJSON_AddArrayToObject(o, "redo");
    for (int i = doc->undo.count - 1, n = 0; i >= 0 && (limit <= 0 || n < limit); i--, n++)
        cJSON_AddItemToArray(undo, cJSON_CreateString(doc->undo.items[i].label));
    for (int i = doc->redo.count - 1, n = 0; i >= 0 && (limit <= 0 || n < limit); i--, n++)
        cJSON_AddItemToArray(redo, cJSON_CreateString(doc->redo.items[i].label));
    cJSON_AddNumberToObject(o, "undo_count", doc->undo.count);
    cJSON_AddNumberToObject(o, "redo_count", doc->redo.count);
    cJSON_AddNumberToObject(o, "memory_bytes", (double) doc->history_bytes);
    return o;
}

// ---------------------------------------------------------------- map form

cJSON *doc_map_form(const Doc *doc) {
    cJSON *m = cJSON_Duplicate(doc->root, 1);
    if (!m) return NULL;
    cJSON *objects = cJSON_GetObjectItemCaseSensitive(m, "objects");
    if (!cJSON_IsArray(objects)) {
        cJSON_DeleteItemFromObjectCaseSensitive(m, "objects");
        objects = cJSON_AddArrayToObject(m, "objects");
    }
    cJSON *spawns = cJSON_GetObjectItemCaseSensitive(m, "spawns");
    if (!cJSON_IsArray(spawns)) {
        cJSON_DeleteItemFromObjectCaseSensitive(m, "spawns");
        spawns = cJSON_AddArrayToObject(m, "spawns");
    }
    // editor form (live projects): tool objects carry objUUID; the export moves them out
    cJSON *o = objects->child;
    while (o) {
        cJSON *next = o->next;
        const int editor_object = doc->editor_form || cJSON_GetObjectItemCaseSensitive(o, "objUUID") != NULL;
        cJSON_DeleteItemFromObjectCaseSensitive(o, "objUUID");
        const int prefab = obj_prefab(o);
        const SchemaPrefab *pf = schema_prefab(prefab);
        if (editor_object && pf && !strcmp(pf->name, "SPAWN_POINT")) {
            double p[3];
            obj_pos(o, p);
            cJSON *s = cJSON_CreateArray();
            for (int k = 0; k < 3; k++) cJSON_AddItemToArray(s, cJSON_CreateNumber(js_round(p[k], 2)));
            cJSON_AddItemToArray(s, cJSON_CreateNumber(js_int(o, "tm", 0)));
            cJSON_AddItemToArray(s, cJSON_CreateNumber(js_int(o, "d", 0)));
            cJSON_AddItemToArray(s, cJSON_CreateNumber(js_get(o, "cp") ? 1 : 0));
            cJSON_AddItemToArray(spawns, s);
            cJSON_Delete(cJSON_DetachItemViaPointer(objects, o));
        } else if (editor_object && pf && !strcmp(pf->name, "CAMERA_POSITION")) {
            cJSON *p = cJSON_DetachItemFromObjectCaseSensitive(o, "p");
            if (p) {
                cJSON_DeleteItemFromObjectCaseSensitive(m, "camPos");
                cJSON_AddItemToObject(m, "camPos", p);
            }
            cJSON_Delete(cJSON_DetachItemViaPointer(objects, o));
        } else if (editor_object && pf && schema_prefab_has_flag(prefab, "noExport")) {
            cJSON_Delete(cJSON_DetachItemViaPointer(objects, o));
        }
        o = next;
    }
    return m;
}
