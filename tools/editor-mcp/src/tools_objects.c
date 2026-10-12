// Object queries and edits.
#define _GNU_SOURCE
#include "mcp.h"
#include "objops.h"
#include "schema.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- shared object builders (objops.h)

int objops_apply_props(Doc *doc, cJSON *obj, const cJSON *props, const int flags, char *err, const size_t errsz) {
    for (const cJSON *k = props ? props->child : NULL; k; k = k->next) {
        if (doc_set_prop(doc, obj, k->string, k, flags, err, errsz) != 0) return -1;
    }
    return 0;
}

static int apply_transform_fields(Doc *doc, cJSON *obj, const cJSON *spec, const int flags, const double snap, char *err, const size_t errsz) {
    static const char *keys[3][2] = {{"p", "position"}, {"s", "size"}, {"r", "rotation"}};
    for (int k = 0; k < 3; k++) {
        const cJSON *v = js_get(spec, keys[k][0]);
        if (!v) v = js_get(spec, keys[k][1]);
        if (!v) continue;
        double d[3];
        if (!js_vec3(v, d)) {
            snprintf(err, errsz, "'%s' needs [x, y, z]", keys[k][1]);
            return -1;
        }
        if (snap > 0 && k < 2) for (int a = 0; a < 3; a++) d[a] = round(d[a] / snap) * snap;
        if (k == 1 && snap > 0) for (int a = 0; a < 3; a++) if (d[a] <= 0) d[a] = snap;
        cJSON *tmp = js_vec3_new(d);
        const int rc = doc_set_prop(doc, obj, keys[k][0], tmp, flags, err, errsz);
        cJSON_Delete(tmp);
        if (rc != 0) return -1;
    }
    const cJSON *deg = js_get(spec, "rotation_deg");
    if (deg) {
        double d[3];
        if (!js_vec3(deg, d)) {
            snprintf(err, errsz, "'rotation_deg' needs [x, y, z]");
            return -1;
        }
        for (int a = 0; a < 3; a++) d[a] *= M_PI / 180.0;
        obj_set_vec(obj, "r", d);
    }
    return 0;
}

cJSON *objops_build(Doc *doc, const cJSON *spec, const int flags, const double snap, char *err, const size_t errsz) {
    const cJSON *pv = js_get(spec, "prefab");
    if (!pv) pv = js_get(spec, "i");
    const int prefab = pv ? schema_prefab_from_json(pv) : 0;
    if (prefab < 0) {
        snprintf(err, errsz, "unknown prefab %s (prefabs tool / krunker://prefabs)", cJSON_IsString(pv) ? pv->valuestring : "(id)");
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    const SchemaPrefab *pf = schema_prefab(prefab);
    double p[3] = {0, 0, 0};
    cJSON_AddItemToObject(obj, "p", js_vec3_new(p));
    cJSON_AddItemToObject(obj, "s", js_vec3_new(pf->default_size));
    if (prefab) cJSON_AddNumberToObject(obj, "i", prefab);
    if (apply_transform_fields(doc, obj, spec, flags, snap, err, errsz) != 0) {
        cJSON_Delete(obj);
        return NULL;
    }
    if (objops_apply_props(doc, obj, js_get(spec, "props"), flags, err, errsz) != 0) {
        cJSON_Delete(obj);
        return NULL;
    }
    const cJSON *raw = js_get(spec, "raw");
    for (const cJSON *k = raw ? raw->child : NULL; k; k = k->next) {
        if (doc_set_prop(doc, obj, k->string, k, flags | SET_RAW_UNKNOWN, err, errsz) != 0) {
            cJSON_Delete(obj);
            return NULL;
        }
    }
    return obj;
}

// A copy of obj as a standalone object: sizes from the xyz pool and palette colours resolved,
// so it can go into another document (or the same one).
cJSON *objops_portable_copy(const Doc *src, const cJSON *obj) {
    cJSON *c = cJSON_Duplicate(obj, 1);
    c->valueint = 0;
    cJSON_DeleteItemFromObjectCaseSensitive(c, "objUUID");
    if (!js_get(c, "s")) {
        double s[3];
        obj_size(src, obj, s);
        cJSON_DeleteItemFromObjectCaseSensitive(c, "si");
        cJSON_AddItemToObject(c, "s", js_vec3_new(s));
    }
    static const char *keys[2] = {"c", "e"};
    for (int k = 0; k < 2; k++) {
        char ikey[4];
        snprintf(ikey, sizeof(ikey), "%si", keys[k]);
        if (!js_get(c, ikey) || js_get(c, keys[k])) continue;
        // ci is also an AI's class index: only palette-resolve when the map has a palette
        if (!cJSON_IsArray(js_get(src->root, "colors")) || schema_prefab_has_flag(obj_prefab(obj), "ai")) continue;
        char hex[8];
        obj_color(src, obj, keys[k], hex);
        cJSON_DeleteItemFromObjectCaseSensitive(c, ikey);
        cJSON_AddStringToObject(c, keys[k], hex);
    }
    return c;
}

// Folds c / e hex colours of a portable copy into dst's palette when dst has one.
void objops_adopt(Doc *dst, cJSON *obj) {
    if (!cJSON_IsArray(js_get(dst->root, "colors"))) return;
    static const char *keys[2] = {"color", "emissive"};
    static const char *raw[2] = {"c", "e"};
    for (int k = 0; k < 2; k++) {
        const cJSON *v = js_get(obj, raw[k]);
        if (!v) continue;
        cJSON *copy = cJSON_Duplicate(v, 1);
        char err[128];
        doc_set_prop(dst, obj, keys[k], copy, SET_FORCE, err, sizeof(err));
        cJSON_Delete(copy);
    }
}

int objops_selection_bounds(Doc *doc, const int *ids, const int n, double mn[3], double mx[3]) {
    for (int k = 0; k < 3; k++) {
        mn[k] = 1e30;
        mx[k] = -1e30;
    }
    int found = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *o = doc_object_by_id(doc, ids[i], NULL);
        if (!o) continue;
        double a[3], b[3];
        obj_aabb(doc, o, a, b);
        for (int k = 0; k < 3; k++) {
            if (a[k] < mn[k]) mn[k] = a[k];
            if (b[k] > mx[k]) mx[k] = b[k];
        }
        found++;
    }
    return found;
}

int objops_check_ids(Doc *doc, const int *ids, const int n, char *err, const size_t errsz) {
    Sb sb = {0};
    int missing = 0;
    for (int i = 0; i < n; i++) {
        if (!doc_object_by_id(doc, ids[i], NULL)) {
            sb_printf(&sb, "%s%d", missing ? ", " : "", ids[i]);
            missing++;
        }
    }
    char *list = sb_take(&sb);
    if (missing) snprintf(err, errsz, "unknown object id(s): %s (objects_query lists ids)", list ? list : "");
    free(list);
    return missing ? -1 : 0;
}

int objops_flags(const cJSON *args) {
    return (js_bool(args, "force", 0) ? SET_FORCE : 0) | (js_bool(args, "clamp", 0) ? SET_CLAMP : 0);
}

cJSON *objops_ids_json(const Edit *e, const int *ids, const int n) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(edit_final_id(e, ids[i])));
    return a;
}

// ---------------------------------------------------------------- query

typedef struct {
    int prefabs[128];
    int prefab_count;
    const char *category, *text, *color, *texture;
    int has_bbox, bbox_inside;
    double bmin[3], bmax[3];
    int has_near;
    double center[3], radius;
    const cJSON *where, *has;
    int collidable, visible, team, iface, target, links; // -2 = any
    int *ids;
    int id_count;
} Filter;

static double point_box_dist(const double p[3], const double mn[3], const double mx[3]) {
    double d2 = 0;
    for (int k = 0; k < 3; k++) {
        const double d = p[k] < mn[k] ? mn[k] - p[k] : p[k] > mx[k] ? p[k] - mx[k] : 0;
        d2 += d * d;
    }
    return sqrt(d2);
}

static int filter_parse(const cJSON *args, Filter *f, char *err, const size_t errsz) {
    memset(f, 0, sizeof(*f));
    f->collidable = f->visible = f->team = f->iface = f->target = f->links = -2;
    const cJSON *pv = js_get(args, "prefab");
    if (pv) {
        if (cJSON_IsArray(pv)) {
            const cJSON *it;
            cJSON_ArrayForEach(it, pv) {
                const int id = schema_prefab_from_json(it);
                if (id < 0) {
                    snprintf(err, errsz, "unknown prefab in filter");
                    return -1;
                }
                if (f->prefab_count < 128) f->prefabs[f->prefab_count++] = id;
            }
        } else {
            const int id = schema_prefab_from_json(pv);
            if (id < 0) {
                snprintf(err, errsz, "unknown prefab '%s'", cJSON_IsString(pv) ? pv->valuestring : "?");
                return -1;
            }
            f->prefabs[f->prefab_count++] = id;
        }
    }
    f->category = js_str(args, "category", NULL);
    f->text = js_str(args, "text", NULL);
    f->color = js_str(args, "color", NULL);
    const cJSON *tex = js_get(args, "texture");
    static char texbuf[16];
    if (cJSON_IsNumber(tex)) {
        snprintf(texbuf, sizeof(texbuf), "%d", (int) tex->valuedouble);
        f->texture = texbuf;
    } else f->texture = js_str(args, "texture", NULL);
    const cJSON *bbox = js_get(args, "bbox");
    if (bbox) {
        if (!js_vec3(js_get(bbox, "min"), f->bmin) || !js_vec3(js_get(bbox, "max"), f->bmax)) {
            snprintf(err, errsz, "bbox needs {min:[x,y,z], max:[x,y,z]}");
            return -1;
        }
        f->has_bbox = 1;
        f->bbox_inside = !strcmp(js_str(bbox, "mode", "intersects"), "inside");
    }
    const cJSON *near = js_get(args, "near");
    if (near) {
        if (!js_vec3(js_get(near, "center"), f->center)) {
            snprintf(err, errsz, "near needs {center:[x,y,z], radius}");
            return -1;
        }
        f->radius = js_num(near, "radius", 50);
        f->has_near = 1;
    }
    f->where = js_get(args, "where");
    f->has = js_get(args, "has");
    if (js_get(args, "collidable")) f->collidable = js_bool(args, "collidable", 1);
    if (js_get(args, "visible")) f->visible = js_bool(args, "visible", 1);
    if (js_get(args, "team")) f->team = js_int(args, "team", 0);
    if (js_get(args, "interface_id")) f->iface = js_int(args, "interface_id", 0);
    if (js_get(args, "target_interface")) f->target = js_int(args, "target_interface", 0);
    if (js_get(args, "links")) f->links = js_int(args, "links", 0);
    if (js_get(args, "ids")) {
        f->ids = tool_ids(js_get(args, "ids"), &f->id_count);
        if (!f->ids) {
            snprintf(err, errsz, "ids needs an array of object ids");
            return -1;
        }
    }
    return 0;
}

static int filter_match(const Doc *doc, const cJSON *o, const Filter *f, double *dist) {
    const int prefab = obj_prefab(o);
    const SchemaPrefab *pf = schema_prefab(prefab);
    if (f->ids) {
        int hit = 0;
        for (int i = 0; i < f->id_count && !hit; i++) hit = f->ids[i] == o->valueint;
        if (!hit) return 0;
    }
    if (f->prefab_count) {
        int hit = 0;
        for (int i = 0; i < f->prefab_count && !hit; i++) hit = f->prefabs[i] == prefab;
        if (!hit) return 0;
    }
    if (f->category && (!pf || strcasecmp(pf->category, f->category))) return 0;
    if (f->text && pf && !strcasestr(pf->name, f->text) && !strcasestr(pf->label, f->text)) return 0;
    double mn[3], mx[3];
    obj_bounds(doc, o, mn, mx);
    if (f->has_bbox) {
        for (int k = 0; k < 3; k++) {
            if (f->bbox_inside ? (mn[k] < f->bmin[k] || mx[k] > f->bmax[k]) : (mx[k] < f->bmin[k] || mn[k] > f->bmax[k])) return 0;
        }
    }
    *dist = point_box_dist(f->center, mn, mx);
    if (f->has_near && *dist > f->radius) return 0;
    if (f->color) {
        char a[8], b[8];
        cJSON *tmp = cJSON_CreateString(f->color);
        const int ok = color_parse(tmp, b);
        cJSON_Delete(tmp);
        obj_color(doc, o, "c", a);
        if (!ok || strcmp(a, b)) return 0;
    }
    if (f->texture) {
        const int want = schema_texture_find(f->texture);
        if (want < 0 || obj_texture(o) != want || !schema_prefab_has_flag(prefab, "texturable")) return 0;
    }
    if (f->collidable != -2 && obj_collidable(o) != f->collidable) return 0;
    if (f->visible != -2 && obj_visible(o) != f->visible) return 0;
    if (f->team != -2 && js_int(o, "tm", 0) != f->team) return 0;
    if (f->iface != -2 && js_int(o, "in", 0) != f->iface) return 0;
    if (f->target != -2 && js_int(o, "tin", 0) != f->target) return 0;
    if (f->links != -2 && js_int(o, "in", 0) != f->links && js_int(o, "tin", 0) != f->links) return 0;
    for (const cJSON *w = f->where ? f->where->child : NULL; w; w = w->next) {
        const cJSON *v = js_get(o, w->string);
        if (cJSON_IsNull(w)) {
            if (v) return 0;
        } else if (cJSON_IsString(w) && !strcmp(w->valuestring, "*")) {
            if (!v) return 0;
        } else if (!v || !cJSON_Compare(v, w, 1)) {
            if (!(v && cJSON_IsNumber(v) && cJSON_IsNumber(w) && v->valuedouble == w->valuedouble)) return 0;
        }
    }
    const cJSON *h;
    cJSON_ArrayForEach(h, f->has) if (cJSON_IsString(h) && !js_get(o, h->valuestring)) return 0;
    return 1;
}

typedef struct {
    const cJSON *obj;
    int index;
    double dist, key;
} Hit;

static int hit_cmp(const void *a, const void *b) {
    const Hit *x = a, *y = b;
    return x->key < y->key ? -1 : x->key > y->key ? 1 : x->index - y->index;
}

static int t_query(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    Filter f;
    char err[512];
    if (filter_parse(args, &f, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    const int n = doc_object_count(p->doc);
    Hit *hits = malloc(sizeof(Hit) * (size_t) (n ? n : 1));
    if (!hits) {
        free(f.ids);
        return tr_error(r, "out of memory");
    }
    const char *sort = tool_str(args, "sort", f.has_near ? "distance" : "index");
    int total = 0, i = 0;
    int *per_prefab = calloc((size_t) schema_prefab_count() + 1, sizeof(int));
    for (const cJSON *o = doc_objects(p->doc)->child; o; o = o->next, i++) {
        double dist = 0;
        if (!filter_match(p->doc, o, &f, &dist)) continue;
        Hit *h = &hits[total++];
        h->obj = o;
        h->index = i;
        h->dist = dist;
        double pos[3], s[3];
        obj_pos(o, pos);
        obj_size(p->doc, o, s);
        h->key = !strcmp(sort, "distance") ? dist : !strcmp(sort, "y") ? pos[1] : !strcmp(sort, "size") ? -(s[0] * s[1] * s[2]) : i;
        if (per_prefab) per_prefab[obj_prefab(o)]++;
    }
    free(f.ids);
    qsort(hits, (size_t) total, sizeof(Hit), hit_cmp);
    int limit = js_int(args, "limit", 100), offset = js_int(args, "offset", 0);
    if (limit < 1) limit = 1;
    if (limit > 5000) limit = 5000;
    if (offset < 0) offset = 0;
    const char *fields = tool_str(args, "fields", "summary");
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "project", p->id);
    cJSON_AddNumberToObject(out, "total", total);
    cJSON *list = cJSON_AddArrayToObject(out, "objects");
    int count = 0;
    for (int k = offset; k < total && count < limit; k++, count++) {
        if (!strcmp(fields, "ids")) {
            cJSON_AddItemToArray(list, cJSON_CreateNumber(hits[k].obj->valueint));
            continue;
        }
        cJSON *o = doc_object_json(p->doc, hits[k].obj, hits[k].index, !strcmp(fields, "full") ? 1 : 0);
        if (f.has_near) cJSON_AddNumberToObject(o, "distance", js_round(hits[k].dist, 2));
        if (!strcmp(fields, "props")) cJSON_AddItemToObject(o, "props", doc_props_json(p->doc, hits[k].obj, 0));
        cJSON_AddItemToArray(list, o);
    }
    cJSON_AddNumberToObject(out, "count", count);
    cJSON_AddNumberToObject(out, "offset", offset);
    if (offset + count < total) cJSON_AddNumberToObject(out, "next_offset", offset + count);
    else cJSON_AddNullToObject(out, "next_offset");
    cJSON *stats = cJSON_AddObjectToObject(out, "by_prefab");
    for (int k = 0; per_prefab && k < schema_prefab_count(); k++) if (per_prefab[k]) cJSON_AddNumberToObject(stats, schema_prefab(k)->name, per_prefab[k]);
    free(per_prefab);
    free(hits);
    tr_json(r, out);
    return 0;
}

static int t_get(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int n;
    int *ids = tool_ids(js_get(args, "id") ? js_get(args, "id") : js_get(args, "ids"), &n);
    if (!ids || !n) {
        free(ids);
        return tr_error(r, "object_get needs id (or ids)");
    }
    cJSON *arr = cJSON_CreateArray();
    char err[512];
    if (objops_check_ids(p->doc, ids, n, err, sizeof(err)) != 0) {
        free(ids);
        cJSON_Delete(arr);
        return tr_error(r, "%s", err);
    }
    for (int i = 0; i < n; i++) {
        int index;
        const cJSON *o = doc_object_by_id(p->doc, ids[i], &index);
        cJSON_AddItemToArray(arr, doc_object_json(p->doc, o, index, js_bool(args, "defaults", 0) ? 2 : 1));
    }
    free(ids);
    if (cJSON_GetArraySize(arr) == 1 && !js_get(args, "ids")) {
        cJSON *one = cJSON_DetachItemFromArray(arr, 0);
        cJSON_Delete(arr);
        tr_json(r, one);
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddItemToObject(o, "objects", arr);
        tr_json(r, o);
    }
    return 0;
}

// ---------------------------------------------------------------- create / update / delete

static int t_create(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const cJSON *list = js_get(args, "objects");
    if (!cJSON_IsArray(list) || !cJSON_GetArraySize(list)) return tr_error(r, "objects_create needs objects: [{prefab, p, s, r, props}]");
    Edit e;
    char err[512];
    char label[64];
    snprintf(label, sizeof(label), "create %d object(s)", cJSON_GetArraySize(list));
    if (edit_begin(ctx, p, tool_str(args, "label", label), &e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    const int flags = objops_flags(args);
    const double snap = js_num(args, "snap", 0);
    const int n = cJSON_GetArraySize(list);
    int *ids = malloc(sizeof(int) * (size_t) n);
    int k = 0;
    const cJSON *spec;
    cJSON_ArrayForEach(spec, list) {
        cJSON *obj = objops_build(p->doc, spec, flags, snap, err, sizeof(err));
        if (!obj) {
            edit_abort(&e);
            free(ids);
            return tr_error(r, "objects[%d]: %s", k, err);
        }
        ids[k++] = edit_add_object(&e, obj, -1);
    }
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "created", objops_ids_json(&e, ids, k));
    cJSON_AddNumberToObject(o, "objects", doc_object_count(p->doc));
    edit_end(&e);
    free(ids);
    tr_json(r, o);
    return 0;
}

static int t_update(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const cJSON *list = js_get(args, "updates");
    if (!cJSON_IsArray(list) || !cJSON_GetArraySize(list)) return tr_error(r, "objects_update needs updates: [{id, p?, s?, r?, prefab?, props?, unset?}]");
    Edit e;
    char err[512], label[64];
    snprintf(label, sizeof(label), "update %d object(s)", cJSON_GetArraySize(list));
    if (edit_begin(ctx, p, tool_str(args, "label", label), &e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    const int flags = objops_flags(args);
    const double snap = js_num(args, "snap", 0);
    int k = 0;
    const cJSON *u;
    cJSON *updated = cJSON_CreateArray();
    cJSON_ArrayForEach(u, list) {
        const int id = js_int(u, "id", 0);
        cJSON *obj = doc_object_by_id(p->doc, id, NULL);
        if (!obj) {
            edit_abort(&e);
            cJSON_Delete(updated);
            return tr_error(r, "updates[%d]: unknown object id %d", k, id);
        }
        const cJSON *pv = js_get(u, "prefab");
        if (pv && doc_set_prop(p->doc, obj, "prefab", pv, flags, err, sizeof(err)) != 0) goto fail;
        if (apply_transform_fields(p->doc, obj, u, flags, snap, err, sizeof(err)) != 0) goto fail;
        if (objops_apply_props(p->doc, obj, js_get(u, "props"), flags, err, sizeof(err)) != 0) goto fail;
        const cJSON *raw = js_get(u, "raw");
        for (const cJSON *rk = raw ? raw->child : NULL; rk; rk = rk->next) {
            if (doc_set_prop(p->doc, obj, rk->string, rk, flags | SET_RAW_UNKNOWN, err, sizeof(err)) != 0) goto fail;
        }
        const cJSON *un;
        cJSON_ArrayForEach(un, js_get(u, "unset")) if (cJSON_IsString(un)) doc_unset_prop(obj, un->valuestring);
        cJSON_AddItemToArray(updated, cJSON_CreateNumber(id));
        k++;
        continue;
    fail:
        edit_abort(&e);
        cJSON_Delete(updated);
        return tr_error(r, "updates[%d] (id %d): %s", k, id, err);
    }
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        cJSON_Delete(updated);
        return tr_error(r, "%s", err);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "updated", updated);
    cJSON_AddBoolToObject(o, "changed", e.changed);
    if (js_bool(args, "return_objects", 0)) {
        cJSON *objs = cJSON_AddArrayToObject(o, "objects");
        const cJSON *id;
        cJSON_ArrayForEach(id, updated) {
            int index;
            const cJSON *ob = doc_object_by_id(p->doc, (int) id->valuedouble, &index);
            if (ob) cJSON_AddItemToArray(objs, doc_object_json(p->doc, ob, index, 1));
        }
    }
    edit_end(&e);
    tr_json(r, o);
    return 0;
}

static int t_delete(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int n;
    int *ids = tool_ids(js_get(args, "ids"), &n);
    if (!ids || !n) {
        free(ids);
        return tr_error(r, "objects_delete needs ids");
    }
    char err[512], label[64];
    if (objops_check_ids(p->doc, ids, n, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    snprintf(label, sizeof(label), "delete %d object(s)", n);
    Edit e;
    if (edit_begin(ctx, p, tool_str(args, "label", label), &e, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    for (int i = 0; i < n; i++) doc_remove_object(p->doc, ids[i]);
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "deleted");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(ids[i]));
    cJSON_AddNumberToObject(o, "objects", doc_object_count(p->doc));
    edit_end(&e);
    free(ids);
    tr_json(r, o);
    return 0;
}

static int t_duplicate(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int n;
    int *ids = tool_ids(js_get(args, "ids"), &n);
    if (!ids || !n) {
        free(ids);
        return tr_error(r, "objects_duplicate needs ids");
    }
    char err[512];
    if (objops_check_ids(p->doc, ids, n, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    double off[3] = {0, 0, 0};
    js_vec3(js_get(args, "offset"), off);
    int count = js_int(args, "count", 1);
    if (count < 1) count = 1;
    if (count * n > 5000) {
        free(ids);
        return tr_error(r, "too many copies (%d)", count * n);
    }
    Edit e;
    char label[64];
    snprintf(label, sizeof(label), "duplicate %d object(s) x%d", n, count);
    if (edit_begin(ctx, p, tool_str(args, "label", label), &e, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    int *created = malloc(sizeof(int) * (size_t) (n * count));
    int c = 0;
    for (int k = 1; k <= count; k++) {
        for (int i = 0; i < n; i++) {
            const cJSON *src = doc_object_by_id(p->doc, ids[i], NULL);
            cJSON *copy = objops_portable_copy(p->doc, src);
            objops_adopt(p->doc, copy);
            double pos[3];
            obj_pos(copy, pos);
            for (int a = 0; a < 3; a++) pos[a] += off[a] * k;
            obj_set_vec(copy, "p", pos);
            created[c++] = edit_add_object(&e, copy, -1);
        }
    }
    free(ids);
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        free(created);
        return tr_error(r, "%s", err);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "created", objops_ids_json(&e, created, c));
    edit_end(&e);
    free(created);
    tr_json(r, o);
    return 0;
}

// ---------------------------------------------------------------- transform

static double axis_of(const char *s) { return s && (s[0] == 'y' || s[0] == 'Y') ? 1 : s && (s[0] == 'z' || s[0] == 'Z') ? 2 : 0; }

// highest collidable top below (x, y, z) inside the footprint, excluding the moving set
static double ground_below(Doc *doc, const double mn[3], const double mx[3], const int *skip, const int nskip) {
    double best = -1e30;
    for (const cJSON *o = doc_objects(doc)->child; o; o = o->next) {
        int skipped = 0;
        for (int i = 0; i < nskip && !skipped; i++) skipped = skip[i] == o->valueint;
        if (skipped || !obj_collidable(o)) continue;
        double a[3], b[3];
        obj_aabb(doc, o, a, b);
        if (b[0] <= mn[0] || a[0] >= mx[0] || b[2] <= mn[2] || a[2] >= mx[2]) continue;
        if (b[1] <= mn[1] + 0.001 && b[1] > best) best = b[1];
    }
    return best;
}

static int t_transform(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int n;
    int *ids = tool_ids(js_get(args, "ids"), &n);
    if (!ids || !n) {
        free(ids);
        return tr_error(r, "objects_transform needs ids");
    }
    char err[512];
    if (objops_check_ids(p->doc, ids, n, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    const char *op = tool_str(args, "op", "");
    double mn[3], mx[3];
    objops_selection_bounds(p->doc, ids, n, mn, mx);
    const double anchor[3] = {(mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2};
    const double center[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
    const char *pivot = tool_str(args, "pivot", "center");
    Edit e;
    char label[96];
    snprintf(label, sizeof(label), "%s %d object(s)", op, n);
    if (edit_begin(ctx, p, tool_str(args, "label", label), &e, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    int *created = NULL, created_n = 0;
    const int flags = objops_flags(args);
#define FAIL(...) do { snprintf(err, sizeof(err), __VA_ARGS__); goto fail; } while (0)
    if (!strcmp(op, "move")) {
        double d[3] = {0, 0, 0}, to[3];
        if (js_vec3(js_get(args, "to"), to)) for (int k = 0; k < 3; k++) d[k] = to[k] - anchor[k];
        else if (!js_vec3(js_get(args, "delta"), d)) FAIL("move needs delta [dx,dy,dz] or to [x,y,z] (the selection's bottom centre)");
        for (int i = 0; i < n; i++) {
            cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
            double pos[3];
            obj_pos(o, pos);
            for (int k = 0; k < 3; k++) pos[k] += d[k];
            obj_set_vec(o, "p", pos);
        }
    } else if (!strcmp(op, "rotate")) {
        double deg[3] = {0, 0, 0};
        if (!js_vec3(js_get(args, "degrees"), deg)) {
            if (cJSON_IsNumber(js_get(args, "degrees"))) deg[1] = js_num(args, "degrees", 0);
            else FAIL("rotate needs degrees [x,y,z] (or a number: around y)");
        }
        const int absolute = !strcmp(tool_str(args, "mode", "relative"), "absolute");
        const double ry = deg[1] * M_PI / 180.0;
        for (int i = 0; i < n; i++) {
            cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
            double rot[3];
            obj_rot(o, rot);
            for (int k = 0; k < 3; k++) rot[k] = absolute ? deg[k] * M_PI / 180.0 : rot[k] + deg[k] * M_PI / 180.0;
            obj_set_vec(o, "r", rot);
            if (!absolute && !strcmp(pivot, "center") && n > 1 && ry != 0) {
                // orbit around the selection centre (y axis; Krunker yaw: x' = x cos + z sin)
                double pos[3];
                obj_pos(o, pos);
                const double dx = pos[0] - center[0], dz = pos[2] - center[2];
                pos[0] = center[0] + dx * cos(ry) + dz * sin(ry);
                pos[2] = center[2] - dx * sin(ry) + dz * cos(ry);
                obj_set_vec(o, "p", pos);
            }
        }
    } else if (!strcmp(op, "scale")) {
        double f[3] = {1, 1, 1}, size[3];
        const int absolute = js_vec3(js_get(args, "size"), size);
        if (!absolute) {
            if (cJSON_IsNumber(js_get(args, "factor"))) f[0] = f[1] = f[2] = js_num(args, "factor", 1);
            else if (!js_vec3(js_get(args, "factor"), f)) FAIL("scale needs factor (number or [fx,fy,fz]) or size [x,y,z]");
        }
        for (int i = 0; i < n; i++) {
            cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
            double s[3], pos[3];
            obj_size(p->doc, o, s);
            obj_pos(o, pos);
            for (int k = 0; k < 3; k++) s[k] = absolute ? size[k] : s[k] * f[k];
            cJSON *tmp = js_vec3_new(s);
            const int rc = doc_set_prop(p->doc, o, "s", tmp, flags, err, sizeof(err));
            cJSON_Delete(tmp);
            if (rc != 0) goto fail;
            if (!absolute && !strcmp(pivot, "center") && n > 1) {
                for (int k = 0; k < 3; k++) pos[k] = anchor[k] + (pos[k] - anchor[k]) * f[k];
                obj_set_vec(o, "p", pos);
            }
        }
    } else if (!strcmp(op, "snap")) {
        const double grid = js_num(args, "grid", 1);
        const double rot_step = js_num(args, "rotation_step_deg", 0) * M_PI / 180.0;
        if (grid <= 0) FAIL("grid must be > 0");
        for (int i = 0; i < n; i++) {
            cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
            double pos[3];
            obj_pos(o, pos);
            for (int k = 0; k < 3; k++) pos[k] = round(pos[k] / grid) * grid;
            obj_set_vec(o, "p", pos);
            if (js_bool(args, "size", 0)) {
                double s[3];
                obj_size(p->doc, o, s);
                for (int k = 0; k < 3; k++) s[k] = fmax(grid, round(s[k] / grid) * grid);
                obj_set_vec(o, "s", s);
            }
            if (rot_step > 0) {
                double rot[3];
                obj_rot(o, rot);
                for (int k = 0; k < 3; k++) rot[k] = round(rot[k] / rot_step) * rot_step;
                obj_set_vec(o, "r", rot);
            }
        }
    } else if (!strcmp(op, "align")) {
        const int ax = (int) axis_of(tool_str(args, "axis", "x"));
        const char *to = tool_str(args, "to", "min");
        double target = !strcmp(to, "max") ? mx[ax] : !strcmp(to, "center") ? (mn[ax] + mx[ax]) / 2 : mn[ax];
        if (cJSON_IsNumber(js_get(args, "value"))) target = js_num(args, "value", 0);
        const char *edge = tool_str(args, "edge", to);
        for (int i = 0; i < n; i++) {
            cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
            double a[3], b[3], pos[3];
            obj_aabb(p->doc, o, a, b);
            obj_pos(o, pos);
            const double cur = !strcmp(edge, "max") ? b[ax] : !strcmp(edge, "center") ? (a[ax] + b[ax]) / 2 : a[ax];
            pos[ax] += target - cur;
            obj_set_vec(o, "p", pos);
        }
    } else if (!strcmp(op, "distribute")) {
        const int ax = (int) axis_of(tool_str(args, "axis", "x"));
        if (n < 3 && !cJSON_IsNumber(js_get(args, "gap"))) FAIL("distribute needs 3+ objects (or a gap)");
        // sort ids by centre on the axis
        double *c = malloc(sizeof(double) * (size_t) n);
        for (int i = 0; i < n; i++) {
            double a[3], b[3];
            obj_aabb(p->doc, doc_object_by_id(p->doc, ids[i], NULL), a, b);
            c[i] = (a[ax] + b[ax]) / 2;
        }
        for (int i = 1; i < n; i++) {
            for (int j = i; j > 0 && c[j] < c[j - 1]; j--) {
                const double tc = c[j]; c[j] = c[j - 1]; c[j - 1] = tc;
                const int ti = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = ti;
            }
        }
        if (cJSON_IsNumber(js_get(args, "gap"))) {
            const double gap = js_num(args, "gap", 0);
            double a[3], b[3];
            obj_aabb(p->doc, doc_object_by_id(p->doc, ids[0], NULL), a, b);
            double cursor = b[ax] + gap;
            for (int i = 1; i < n; i++) {
                cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
                double pos[3];
                obj_aabb(p->doc, o, a, b);
                obj_pos(o, pos);
                pos[ax] += cursor - a[ax];
                obj_set_vec(o, "p", pos);
                cursor += (b[ax] - a[ax]) + gap;
            }
        } else {
            const double first = c[0], step = (c[n - 1] - c[0]) / (n - 1);
            for (int i = 1; i < n - 1; i++) {
                cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
                double pos[3];
                obj_pos(o, pos);
                pos[ax] += first + step * i - c[i];
                obj_set_vec(o, "p", pos);
            }
        }
        free(c);
    } else if (!strcmp(op, "mirror")) {
        const int ax = (int) axis_of(tool_str(args, "axis", "x"));
        const double plane = cJSON_IsNumber(js_get(args, "plane")) ? js_num(args, "plane", 0) : center[ax];
        const int copy = js_bool(args, "copy", 0);
        if (copy) created = malloc(sizeof(int) * (size_t) n);
        for (int i = 0; i < n; i++) {
            cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
            if (copy) {
                o = objops_portable_copy(p->doc, o);
                objops_adopt(p->doc, o);
            }
            double pos[3], rot[3];
            obj_pos(o, pos);
            obj_rot(o, rot);
            pos[ax] = 2 * plane - pos[ax];
            if (ax == 0) { rot[1] = -rot[1]; rot[2] = -rot[2]; }
            else if (ax == 2) { rot[0] = -rot[0]; rot[1] = -rot[1]; }
            else { rot[0] = -rot[0]; rot[2] = -rot[2]; }
            obj_set_vec(o, "p", pos);
            obj_set_vec(o, "r", rot);
            const cJSON *d = js_get(o, "d");
            if (cJSON_IsNumber(d) && ax != 1) {
                const int dv = (int) d->valuedouble;
                const int nd = ax == 0 ? ((2 - dv) % 4 + 4) % 4 : ((4 - dv) % 4 + 4) % 4;
                cJSON_SetNumberValue((cJSON *) d, nd);
                if (!nd) cJSON_DeleteItemFromObjectCaseSensitive(o, "d");
            }
            if (copy) created[created_n++] = edit_add_object(&e, o, -1);
        }
    } else if (!strcmp(op, "drop")) {
        for (int i = 0; i < n; i++) {
            cJSON *o = doc_object_by_id(p->doc, ids[i], NULL);
            double a[3], b[3], pos[3];
            obj_aabb(p->doc, o, a, b);
            obj_pos(o, pos);
            const double g = ground_below(p->doc, a, b, ids, n);
            if (g > -1e29) {
                pos[1] = g;
                obj_set_vec(o, "p", pos);
            }
        }
    } else {
        FAIL("unknown op '%s' (move, rotate, scale, snap, align, distribute, mirror, drop)", op);
    }
#undef FAIL
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        free(ids);
        free(created);
        return tr_error(r, "%s", err);
    }
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "op", op);
        cJSON_AddItemToObject(o, "ids", objops_ids_json(&e, ids, n));
        if (created_n) cJSON_AddItemToObject(o, "created", objops_ids_json(&e, created, created_n));
        double a[3], b[3];
        if (p->kind == PROJ_FILE && objops_selection_bounds(p->doc, ids, n, a, b)) {
            cJSON *bb = cJSON_AddObjectToObject(o, "bounds");
            cJSON_AddItemToObject(bb, "min", js_vec3_new(a));
            cJSON_AddItemToObject(bb, "max", js_vec3_new(b));
        }
        edit_end(&e);
        free(ids);
        free(created);
        tr_json(r, o);
    }
    return 0;
fail:
    edit_abort(&e);
    free(ids);
    free(created);
    return tr_error(r, "%s", err);
}

// ---------------------------------------------------------------- cross-project copy

static int t_copy_between(Ctx *ctx, const cJSON *args, ToolResult *r) {
    const char *from = tool_str(args, "from", NULL), *to = tool_str(args, "to", NULL);
    if (!from || !to) return tr_error(r, "objects_copy_between_projects needs from and to");
    Project *src = NULL, *dst = NULL;
    char err[512];
    if (project_resolve(ctx, from, &src, err, sizeof(err)) != 0) return tr_error(r, "from: %s", err);
    if (project_resolve(ctx, to, &dst, err, sizeof(err)) != 0) return tr_error(r, "to: %s", err);
    int n;
    int *ids = tool_ids(js_get(args, "ids"), &n);
    if (!ids || !n) {
        free(ids);
        return tr_error(r, "objects_copy_between_projects needs ids (from the source project)");
    }
    if (objops_check_ids(src->doc, ids, n, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "from: %s", err);
    }
    double mn[3], mx[3], off[3] = {0, 0, 0}, at[3];
    objops_selection_bounds(src->doc, ids, n, mn, mx);
    if (js_vec3(js_get(args, "at"), at)) {
        off[0] = at[0] - (mn[0] + mx[0]) / 2;
        off[1] = at[1] - mn[1];
        off[2] = at[2] - (mn[2] + mx[2]) / 2;
    } else js_vec3(js_get(args, "offset"), off);
    // portable copies first (the source may be the target)
    cJSON **copies = calloc((size_t) n, sizeof(cJSON *));
    for (int i = 0; i < n; i++) copies[i] = objops_portable_copy(src->doc, doc_object_by_id(src->doc, ids[i], NULL));
    free(ids);
    Edit e;
    char label[96];
    snprintf(label, sizeof(label), "copy %d object(s) from %s", n, src->name);
    if (edit_begin(ctx, dst, tool_str(args, "label", label), &e, err, sizeof(err)) != 0) {
        for (int i = 0; i < n; i++) cJSON_Delete(copies[i]);
        free(copies);
        return tr_error(r, "%s", err);
    }
    int *created = malloc(sizeof(int) * (size_t) n);
    for (int i = 0; i < n; i++) {
        double pos[3];
        obj_pos(copies[i], pos);
        for (int k = 0; k < 3; k++) pos[k] += off[k];
        obj_set_vec(copies[i], "p", pos);
        objops_adopt(dst->doc, copies[i]);
        created[i] = edit_add_object(&e, copies[i], -1);
    }
    free(copies);
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        free(created);
        return tr_error(r, "%s", err);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "from", src->id);
    cJSON_AddStringToObject(o, "to", dst->id);
    cJSON_AddItemToObject(o, "created", objops_ids_json(&e, created, n));
    cJSON_AddNumberToObject(o, "objects", doc_object_count(dst->doc));
    edit_end(&e);
    free(created);
    tr_json(r, o);
    return 0;
}

#define OBJ_SPEC {"type":"object","properties":{"prefab":{"type":["string","integer"],"description":"prefab name or id (default CUBE)"},"p":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"bottom-centre position"},"s":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"size (default: the prefab's)"},"r":{"type":"array","items":{"type":"number"},"description":"rotation, radians"},"rotation_deg":{"type":"array","items":{"type":"number"}},"props":{"type":"object","description":"properties by friendly name or map key: color, texture, collidable, visible, opacity, interface_id, ..."},"raw":{"type":"object","description":"raw map keys copied as-is (validated when known)"}}}

const ToolDef g_tools_objects[] = {
    {"objects_query", "Query objects",
     "Find objects with filters (all optional, combined with AND): ids, prefab (name/id or list), category, text (prefab name), "
     "bbox {min,max,mode:intersects|inside}, near {center,radius} (sorted by distance), where {key: value | null (absent) | \"*\" "
     "(present)}, has [keys], color, texture, collidable, visible, team, interface_id, target_interface, links (in or tin). "
     "fields: summary (default), full (+props, raw, bounds), props, ids. Paginated with limit / offset (next_offset); by_prefab "
     "counts the whole match.",
     S({"type":"object","properties":{PROJECT_ARG,"ids":{"type":"array","items":{"type":"integer"}},"prefab":{},"category":{"type":"string"},"text":{"type":"string"},"bbox":{"type":"object","properties":{"min":{"type":"array"},"max":{"type":"array"},"mode":{"type":"string","enum":["intersects","inside"]}}},"near":{"type":"object","properties":{"center":{"type":"array"},"radius":{"type":"number"}}},"where":{"type":"object"},"has":{"type":"array","items":{"type":"string"}},"color":{"type":"string"},"texture":{"type":["string","integer"]},"collidable":{"type":"boolean"},"visible":{"type":"boolean"},"team":{"type":"integer"},"interface_id":{"type":"integer"},"target_interface":{"type":"integer"},"links":{"type":"integer"},"fields":{"type":"string","enum":["summary","full","props","ids"]},"sort":{"type":"string","enum":["index","distance","y","size"]},"limit":{"type":"integer","default":100},"offset":{"type":"integer","default":0}}}),
     1, 0, 1, 0, t_query},
    {"object_get", "Get object(s)",
     "Full detail of one object (id) or several (ids): prefab, transform, bounds, decoded properties (defaults:true adds every "
     "property the prefab accepts with its default), unknown keys and the raw map JSON.",
     S({"type":"object","properties":{PROJECT_ARG,"id":{"type":"integer"},"ids":{"type":"array","items":{"type":"integer"}},"defaults":{"type":"boolean"}}}),
     1, 0, 1, 0, t_get},
    {"objects_create", "Create objects",
     "Create objects in one undo step. Each: {prefab, p, s, r | rotation_deg, props, raw}. Properties are validated against the "
     "prefab (prefabs tool): unknown or unavailable ones fail unless force:true; out-of-range values fail unless clamp:true. "
     "snap rounds p and s to a grid. Returns the new ids.",
     S({"type":"object","properties":{PROJECT_ARG,"objects":{"type":"array","items":OBJ_SPEC},"snap":{"type":"number"},"force":{"type":"boolean"},"clamp":{"type":"boolean"},"label":{"type":"string"}},"required":["objects"]}),
     0, 0, 0, 0, t_create},
    {"objects_update", "Update objects",
     "Batched edits in one undo step: updates [{id, p?, s?, r? | rotation_deg?, prefab?, props?: {name: value | null}, raw?, "
     "unset?: [names]}]. Same validation as objects_create; return_objects:true returns the edited objects.",
     S({"type":"object","properties":{PROJECT_ARG,"updates":{"type":"array","items":{"type":"object","properties":{"id":{"type":"integer"},"p":{"type":"array"},"s":{"type":"array"},"r":{"type":"array"},"rotation_deg":{"type":"array"},"prefab":{},"props":{"type":"object"},"raw":{"type":"object"},"unset":{"type":"array","items":{"type":"string"}}},"required":["id"]}},"snap":{"type":"number"},"force":{"type":"boolean"},"clamp":{"type":"boolean"},"return_objects":{"type":"boolean"},"label":{"type":"string"}},"required":["updates"]}),
     0, 0, 0, 0, t_update},
    {"objects_delete", "Delete objects", "Delete objects by id (one undo step).",
     S({"type":"object","properties":{PROJECT_ARG,"ids":{"type":"array","items":{"type":"integer"}},"label":{"type":"string"}},"required":["ids"]}),
     0, 1, 0, 0, t_delete},
    {"objects_duplicate", "Duplicate objects",
     "Copy objects count times, each copy shifted by offset * k (an array of copies). One undo step; returns the new ids.",
     S({"type":"object","properties":{PROJECT_ARG,"ids":{"type":"array","items":{"type":"integer"}},"offset":{"type":"array","items":{"type":"number"}},"count":{"type":"integer","default":1},"label":{"type":"string"}},"required":["ids"]}),
     0, 0, 0, 0, t_duplicate},
    {"objects_transform", "Transform objects",
     "Transform a selection in one undo step. op: move (delta | to = new bottom centre of the selection), rotate (degrees "
     "[x,y,z] or number = yaw; mode relative|absolute; pivot center orbits positions around the selection for yaw), scale (factor "
     "number|[x,y,z] or size [x,y,z]; pivot center scales positions too), snap (grid, size:true snaps sizes, rotation_step_deg), "
     "align (axis, to min|center|max of the selection or value, edge), distribute (axis, evenly by centre or gap between bounds), "
     "mirror (axis, plane default selection centre, copy:true mirrors copies; directions follow), drop (onto the highest "
     "collidable surface below).",
     S({"type":"object","properties":{PROJECT_ARG,"ids":{"type":"array","items":{"type":"integer"}},"op":{"type":"string","enum":["move","rotate","scale","snap","align","distribute","mirror","drop"]},"delta":{"type":"array"},"to":{},"degrees":{},"mode":{"type":"string","enum":["relative","absolute"]},"pivot":{"type":"string","enum":["center","each"]},"factor":{},"size":{},"grid":{"type":"number"},"rotation_step_deg":{"type":"number"},"axis":{"type":"string","enum":["x","y","z"]},"value":{"type":"number"},"edge":{"type":"string","enum":["min","center","max"]},"gap":{"type":"number"},"plane":{"type":"number"},"copy":{"type":"boolean"},"force":{"type":"boolean"},"clamp":{"type":"boolean"},"label":{"type":"string"}},"required":["ids","op"]}),
     0, 0, 0, 0, t_transform},
    {"objects_copy_between_projects", "Copy objects between projects",
     "Copy objects from one project to another (file or live editor projects, any mix): palette colours and pooled sizes are "
     "resolved, positions kept plus offset, or the selection's bottom centre placed at `at`. One undo step in the target.",
     S({"type":"object","properties":{"from":{"type":"string"},"to":{"type":"string"},"ids":{"type":"array","items":{"type":"integer"}},"offset":{"type":"array","items":{"type":"number"}},"at":{"type":"array","items":{"type":"number"}},"label":{"type":"string"}},"required":["from","to","ids"]}),
     0, 0, 0, 0, t_copy_between},
    {0},
};
