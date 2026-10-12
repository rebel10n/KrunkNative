// Grouping, the prefab library and procedural generators. Everything creates objects through
// objops_build / edit_add_object, so it validates and undoes like objects_create.
#define _GNU_SOURCE
#include "jsonw.h"
#include "mcp.h"
#include "objops.h"
#include "schema.h"
#include "util.h"
#include "validate.h"
#include <pcg_basic.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- groups (session)

static int valid_name(const char *s) {
    if (!s || !s[0] || strlen(s) > 64) return 0;
    for (const char *c = s; *c; c++) if (!(isalnum((unsigned char) *c) || *c == '_' || *c == '-' || *c == '.')) return 0;
    return s[0] != '.';
}

static int t_group_create(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const char *name = tool_str(args, "name", NULL);
    if (!valid_name(name)) return tr_error(r, "group_create needs name ([A-Za-z0-9_.-], <= 64)");
    int n;
    int *ids = tool_ids(js_get(args, "ids"), &n);
    char err[512];
    if (!ids || !n || objops_check_ids(p->doc, ids, n, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", ids ? err : "group_create needs ids");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(p->groups, name);
    cJSON *g = cJSON_AddObjectToObject(p->groups, name);
    cJSON *arr = cJSON_AddArrayToObject(g, "ids");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(ids[i]));
    double mn[3], mx[3];
    objops_selection_bounds(p->doc, ids, n, mn, mx);
    free(ids);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "group", name);
    cJSON_AddNumberToObject(o, "count", n);
    cJSON *b = cJSON_AddObjectToObject(o, "bounds");
    cJSON_AddItemToObject(b, "min", js_vec3_new(mn));
    cJSON_AddItemToObject(b, "max", js_vec3_new(mx));
    tr_json(r, o);
    return 0;
}

static int *group_ids(Project *p, const char *name, int *n) {
    const cJSON *g = js_get(p->groups, name);
    if (!g) return NULL;
    // drop ids of deleted objects
    int count = 0;
    int *ids = tool_ids(js_get(g, "ids"), &count);
    int k = 0;
    for (int i = 0; ids && i < count; i++) if (doc_object_by_id(p->doc, ids[i], NULL)) ids[k++] = ids[i];
    *n = k;
    return ids;
}

static int t_group_list(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "groups");
    for (const cJSON *g = p->groups->child; g; g = g->next) {
        int n;
        int *ids = group_ids(p, g->string, &n);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", g->string);
        cJSON *a = cJSON_AddArrayToObject(e, "ids");
        for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(ids[i]));
        free(ids);
        cJSON_AddItemToArray(arr, e);
    }
    tr_json(r, o);
    return 0;
}

static int t_group_delete(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const char *name = tool_str(args, "name", "");
    if (!js_get(p->groups, name)) return tr_error(r, "no group '%s'", name);
    int n;
    int *ids = group_ids(p, name, &n);
    char err[512];
    if (js_bool(args, "delete_objects", 0) && n) {
        Edit e;
        if (edit_begin(ctx, p, "delete group objects", &e, err, sizeof(err)) != 0) {
            free(ids);
            return tr_error(r, "%s", err);
        }
        for (int i = 0; i < n; i++) doc_remove_object(p->doc, ids[i]);
        if (edit_commit(&e, err, sizeof(err)) != 0) {
            free(ids);
            return tr_error(r, "%s", err);
        }
        edit_end(&e);
    }
    free(ids);
    cJSON_DeleteItemFromObjectCaseSensitive(p->groups, name);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "removed", name);
    cJSON_AddBoolToObject(o, "objects_deleted", js_bool(args, "delete_objects", 0));
    tr_json(r, o);
    return 0;
}

// copies of `ids` placed so the selection's bottom centre lands at anchor + step * k, yawed
static int stamp(Edit *e, Doc *src, const int *ids, const int n, const double at[3], const double step[3], const int count,
                 const double yaw_deg, cJSON *created, char *err, const size_t errsz) {
    double mn[3], mx[3];
    objops_selection_bounds(src, ids, n, mn, mx);
    const double anchor[3] = {(mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2};
    const double yaw = yaw_deg * M_PI / 180.0;
    const int quarter = fabs(fmod(yaw_deg, 90.0)) < 1e-9;
    for (int k = 0; k < count; k++) {
        for (int i = 0; i < n; i++) {
            const cJSON *o = doc_object_by_id(src, ids[i], NULL);
            if (!o) continue;
            cJSON *c = objops_portable_copy(src, o);
            objops_adopt(e->p->doc, c);
            double pos[3], rot[3], size[3];
            obj_pos(c, pos);
            obj_rot(c, rot);
            obj_size(src, c, size);
            const double dx = pos[0] - anchor[0], dz = pos[2] - anchor[2];
            pos[0] = at[0] + step[0] * k + dx * cos(yaw) + dz * sin(yaw);
            pos[1] = at[1] + step[1] * k + (pos[1] - anchor[1]);
            pos[2] = at[2] + step[2] * k - dx * sin(yaw) + dz * cos(yaw);
            obj_set_vec(c, "p", pos);
            if (quarter && ((int) lround(yaw_deg / 90.0)) % 2) {
                // a quarter turn swaps the footprint instead of rotating the box (collision is axis aligned)
                const double t = size[0];
                size[0] = size[2];
                size[2] = t;
                obj_set_vec(c, "s", size);
            } else if (!quarter) {
                rot[1] += yaw;
                obj_set_vec(c, "r", rot);
            }
            const cJSON *d = js_get(c, "d");
            if (quarter && cJSON_IsNumber(d)) {
                const int nd = (((int) d->valuedouble + (int) lround(yaw_deg / 90.0)) % 4 + 4) % 4;
                cJSON_DeleteItemFromObjectCaseSensitive(c, "d");
                if (nd) cJSON_AddNumberToObject(c, "d", nd);
            }
            cJSON_AddItemToArray(created, cJSON_CreateNumber(edit_add_object(e, c, -1)));
        }
    }
    (void) err;
    (void) errsz;
    return 0;
}

static void final_ids(const Edit *e, cJSON *ids) {
    for (cJSON *it = ids->child; it; it = it->next) cJSON_SetNumberValue(it, edit_final_id(e, (int) it->valuedouble));
}

static int t_group_stamp(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int n = 0;
    int *ids = tool_str(args, "name", NULL) ? group_ids(p, tool_str(args, "name", ""), &n) : tool_ids(js_get(args, "ids"), &n);
    char err[512];
    if (!ids || !n) {
        free(ids);
        return tr_error(r, "group_stamp needs name (a group) or ids");
    }
    double at[3], step[3] = {0, 0, 0}, mn[3], mx[3];
    objops_selection_bounds(p->doc, ids, n, mn, mx);
    if (!js_vec3(js_get(args, "at"), at)) {
        double off[3] = {mx[0] - mn[0] + 4, 0, 0};
        js_vec3(js_get(args, "offset"), off);
        at[0] = (mn[0] + mx[0]) / 2 + off[0];
        at[1] = mn[1] + off[1];
        at[2] = (mn[2] + mx[2]) / 2 + off[2];
    }
    js_vec3(js_get(args, "step"), step);
    int count = js_int(args, "count", 1);
    if (count < 1) count = 1;
    if (count * n > 5000) {
        free(ids);
        return tr_error(r, "too many copies");
    }
    Edit e;
    if (edit_begin(ctx, p, "stamp group", &e, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", err);
    }
    cJSON *created = cJSON_CreateArray();
    stamp(&e, p->doc, ids, n, at, step, count, js_num(args, "rotate_y_deg", 0), created, err, sizeof(err));
    free(ids);
    if (edit_commit(&e, err, sizeof(err)) != 0) {
        cJSON_Delete(created);
        return tr_error(r, "%s", err);
    }
    final_ids(&e, created);
    edit_end(&e);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "created", created);
    if (tool_str(args, "new_group", NULL) && valid_name(tool_str(args, "new_group", ""))) {
        cJSON_DeleteItemFromObjectCaseSensitive(p->groups, tool_str(args, "new_group", ""));
        cJSON *g = cJSON_AddObjectToObject(p->groups, tool_str(args, "new_group", ""));
        cJSON_AddItemToObject(g, "ids", cJSON_Duplicate(created, 1));
    }
    tr_json(r, o);
    return 0;
}

// ---------------------------------------------------------------- prefab library (workspace/prefabs/*.json)

static void library_path(const Ctx *ctx, const char *name, char *out, const size_t n) {
    snprintf(out, n, "%s/prefabs/%s.json", ctx->workspace, name);
}

static int t_prefab_save(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const char *name = tool_str(args, "name", NULL);
    if (!valid_name(name)) return tr_error(r, "prefab_save needs name ([A-Za-z0-9_.-])");
    int n = 0;
    int *ids = tool_str(args, "group", NULL) ? group_ids(p, tool_str(args, "group", ""), &n) : tool_ids(js_get(args, "ids"), &n);
    char err[512];
    if (!ids || !n || objops_check_ids(p->doc, ids, n, err, sizeof(err)) != 0) {
        free(ids);
        return tr_error(r, "%s", ids ? err : "prefab_save needs ids or group");
    }
    double mn[3], mx[3];
    objops_selection_bounds(p->doc, ids, n, mn, mx);
    const double anchor[3] = {(mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2};
    cJSON *lib = cJSON_CreateObject();
    cJSON_AddStringToObject(lib, "name", name);
    cJSON_AddStringToObject(lib, "description", tool_str(args, "description", ""));
    cJSON_AddStringToObject(lib, "source", p->name);
    const double size[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
    cJSON_AddItemToObject(lib, "size", js_vec3_new(size));
    cJSON *objs = cJSON_AddArrayToObject(lib, "objects");
    for (int i = 0; i < n; i++) {
        cJSON *c = objops_portable_copy(p->doc, doc_object_by_id(p->doc, ids[i], NULL));
        double pos[3];
        obj_pos(c, pos);
        for (int k = 0; k < 3; k++) pos[k] -= anchor[k];
        obj_set_vec(c, "p", pos);
        cJSON_AddItemToArray(objs, c);
    }
    free(ids);
    char path[PATH_MAX + 64], dir[PATH_MAX + 16];
    snprintf(dir, sizeof(dir), "%s/prefabs", ctx->workspace);
    mkdir_p(dir);
    library_path(ctx, name, path, sizeof(path));
    char *text = jsonw_print(lib, JSONW_PRETTY);
    const int rc = text ? file_write(path, text, strlen(text)) : -1;
    free(text);
    cJSON_Delete(lib);
    if (rc != 0) return tr_error(r, "cannot write %s", path);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "prefab", name);
    cJSON_AddStringToObject(o, "path", path);
    cJSON_AddNumberToObject(o, "objects", n);
    tr_json(r, o);
    return 0;
}

static int t_prefab_list(Ctx *ctx, const cJSON *args, ToolResult *r) {
    (void) args;
    char dir[PATH_MAX + 16];
    snprintf(dir, sizeof(dir), "%s/prefabs", ctx->workspace);
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "prefabs");
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const size_t n = strlen(e->d_name);
            if (n < 6 || strcmp(e->d_name + n - 5, ".json")) continue;
            char path[PATH_MAX + 300];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            char *text = file_read(path, NULL);
            cJSON *lib = text ? cJSON_Parse(text) : NULL;
            free(text);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", js_str(lib, "name", ""));
            cJSON_AddStringToObject(item, "description", js_str(lib, "description", ""));
            cJSON_AddNumberToObject(item, "objects", cJSON_GetArraySize(js_get(lib, "objects")));
            if (js_get(lib, "size")) cJSON_AddItemToObject(item, "size", cJSON_Duplicate(js_get(lib, "size"), 1));
            cJSON_AddItemToArray(arr, item);
            cJSON_Delete(lib);
        }
        closedir(d);
    }
    cJSON_AddStringToObject(o, "dir", dir);
    tr_json(r, o);
    return 0;
}

static int t_prefab_stamp(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const char *name = tool_str(args, "name", NULL);
    if (!valid_name(name)) return tr_error(r, "prefab_stamp needs name (prefab_list)");
    char path[PATH_MAX + 64], err[512];
    library_path(ctx, name, path, sizeof(path));
    char *text = file_read(path, NULL);
    cJSON *lib = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!lib) return tr_error(r, "no library prefab '%s' (%s)", name, path);
    // the library objects as a scratch document, stamped like a group
    Doc *scratch = calloc(1, sizeof(Doc));
    scratch->root = cJSON_CreateObject();
    scratch->next_uid = 1;
    cJSON_AddItemToObject(scratch->root, "objects", cJSON_DetachItemFromObjectCaseSensitive(lib, "objects"));
    doc_assign_missing_ids(scratch);
    cJSON_Delete(lib);
    const int n = doc_object_count(scratch);
    int *ids = malloc(sizeof(int) * (size_t) (n ? n : 1));
    int k = 0;
    for (cJSON *o = doc_objects(scratch)->child; o; o = o->next) ids[k++] = o->valueint;
    double at[3] = {0, 0, 0}, step[3] = {0, 0, 0};
    js_vec3(js_get(args, "at"), at);
    js_vec3(js_get(args, "step"), step);
    int count = js_int(args, "count", 1);
    if (count < 1) count = 1;
    Edit e;
    cJSON *created = cJSON_CreateArray();
    int rc = edit_begin(ctx, p, "stamp prefab", &e, err, sizeof(err));
    if (rc == 0) {
        stamp(&e, scratch, ids, n, at, step, count, js_num(args, "rotate_y_deg", 0), created, err, sizeof(err));
        rc = edit_commit(&e, err, sizeof(err));
    }
    free(ids);
    doc_free(scratch);
    if (rc != 0) {
        cJSON_Delete(created);
        return tr_error(r, "%s", err);
    }
    final_ids(&e, created);
    edit_end(&e);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "created", created);
    tr_json(r, o);
    return 0;
}

// ---------------------------------------------------------------- generators

typedef struct {
    Edit e;
    Project *p;
    cJSON *created;
    const cJSON *style; // props applied to every generated object
    int flags;
    char err[512];
    int failed;
} Gen;

static void gen_box(Gen *g, const char *prefab, const double c[3], const double s[3], const cJSON *extra) {
    if (g->failed || s[0] <= 0.001 || s[1] <= 0.001 || s[2] <= 0.001) return;
    cJSON *spec = cJSON_CreateObject();
    cJSON_AddStringToObject(spec, "prefab", prefab);
    cJSON_AddItemToObject(spec, "p", js_vec3_new(c));
    cJSON_AddItemToObject(spec, "s", js_vec3_new(s));
    cJSON *props = cJSON_Duplicate(g->style ? g->style : NULL, 1);
    if (!props) props = cJSON_CreateObject();
    for (const cJSON *k = extra ? extra->child : NULL; k; k = k->next) {
        cJSON_DeleteItemFromObjectCaseSensitive(props, k->string);
        cJSON_AddItemToObject(props, k->string, cJSON_Duplicate(k, 1));
    }
    cJSON_AddItemToObject(spec, "props", props);
    cJSON *obj = objops_build(g->p->doc, spec, g->flags, 0, g->err, sizeof(g->err));
    cJSON_Delete(spec);
    if (!obj) {
        g->failed = 1;
        return;
    }
    cJSON_AddItemToArray(g->created, cJSON_CreateNumber(edit_add_object(&g->e, obj, -1)));
}

static int gen_begin(Ctx *ctx, const cJSON *args, ToolResult *r, Gen *g, const char *label) {
    memset(g, 0, sizeof(*g));
    g->p = tool_project(ctx, args, r);
    if (!g->p) return -1;
    g->style = js_get(args, "props");
    g->flags = objops_flags(args);
    g->created = cJSON_CreateArray();
    if (edit_begin(ctx, g->p, tool_str(args, "label", label), &g->e, g->err, sizeof(g->err)) != 0) {
        cJSON_Delete(g->created);
        tr_error(r, "%s", g->err);
        return -1;
    }
    return 0;
}

static int gen_end(Gen *g, ToolResult *r, cJSON *extra) {
    if (g->failed) {
        edit_abort(&g->e);
        cJSON_Delete(g->created);
        cJSON_Delete(extra);
        return tr_error(r, "%s", g->err);
    }
    if (edit_commit(&g->e, g->err, sizeof(g->err)) != 0) {
        cJSON_Delete(g->created);
        cJSON_Delete(extra);
        return tr_error(r, "%s", g->err);
    }
    final_ids(&g->e, g->created);
    edit_end(&g->e);
    cJSON *o = extra ? extra : cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "count", cJSON_GetArraySize(g->created));
    cJSON_AddItemToObject(o, "created", g->created);
    tr_json(r, o);
    return 0;
}

// a wall along one side with door openings cut out
typedef struct { double offset, width, height; } Door;

static void gen_wall(Gen *g, const int along_x, const double fixed, const double a0, const double a1, const double y, const double h,
                     const double t, const Door *doors, const int nd) {
    // sort doors by offset
    Door ds[16];
    int n = nd < 16 ? nd : 16;
    memcpy(ds, doors, sizeof(Door) * (size_t) n);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (ds[j].offset < ds[i].offset) { const Door tmp = ds[i]; ds[i] = ds[j]; ds[j] = tmp; }
    const double mid = (a0 + a1) / 2;
    double cursor = a0;
    for (int i = 0; i <= n; i++) {
        const double d0 = i < n ? mid + ds[i].offset - ds[i].width / 2 : a1;
        const double d1 = i < n ? mid + ds[i].offset + ds[i].width / 2 : a1;
        if (d0 > cursor) {
            const double len = d0 - cursor, c = (cursor + d0) / 2;
            const double pos[3] = {along_x ? c : fixed, y, along_x ? fixed : c};
            const double size[3] = {along_x ? len : t, h, along_x ? t : len};
            gen_box(g, "CUBE", pos, size, NULL);
        }
        if (i < n && ds[i].height < h) {
            const double c = (d0 + d1) / 2;
            const double pos[3] = {along_x ? c : fixed, y + ds[i].height, along_x ? fixed : c};
            const double size[3] = {along_x ? d1 - d0 : t, h - ds[i].height, along_x ? t : d1 - d0};
            gen_box(g, "CUBE", pos, size, NULL);
        }
        if (d1 > cursor) cursor = d1;
    }
}

static int t_box_room(Ctx *ctx, const cJSON *args, ToolResult *r) {
    double c[3] = {0, 0, 0}, s[3] = {100, 30, 100};
    js_vec3(js_get(args, "center"), c);
    js_vec3(js_get(args, "size"), s);
    if (s[0] < 4 || s[1] < 4 || s[2] < 4) return tr_error(r, "size must be at least 4 on every axis");
    const double t = fmax(0.5, js_num(args, "wall", 2));
    Gen g;
    if (gen_begin(ctx, args, r, &g, "generate box room") != 0) return 0;
    const double x0 = c[0] - s[0] / 2, x1 = c[0] + s[0] / 2, z0 = c[2] - s[2] / 2, z1 = c[2] + s[2] / 2;
    if (js_bool(args, "floor", 1)) {
        const double p[3] = {c[0], c[1] - t, c[2]}, sz[3] = {s[0], t, s[2]};
        gen_box(&g, "CUBE", p, sz, js_get(args, "floor_props"));
    }
    if (js_bool(args, "ceiling", 0)) {
        const double p[3] = {c[0], c[1] + s[1], c[2]}, sz[3] = {s[0], t, s[2]};
        gen_box(&g, "CUBE", p, sz, NULL);
    }
    // doors per side: north (-Z), south (+Z), west (-X), east (+X)
    Door doors[4][16];
    int nd[4] = {0};
    static const char *sides[4] = {"north", "south", "west", "east"};
    const cJSON *d;
    cJSON_ArrayForEach(d, js_get(args, "doors")) {
        const char *side = js_str(d, "side", "south");
        int si = -1;
        for (int i = 0; i < 4; i++) if (!strcasecmp(side, sides[i])) si = i;
        if (si < 0 || nd[si] >= 16) continue;
        doors[si][nd[si]++] = (Door) {js_num(d, "offset", 0), js_num(d, "width", 12), js_num(d, "height", 16)};
    }
    // north / south walls span the full width (corners), west / east fit between them
    gen_wall(&g, 1, z0 + t / 2, x0, x1, c[1], s[1], t, doors[0], nd[0]);
    gen_wall(&g, 1, z1 - t / 2, x0, x1, c[1], s[1], t, doors[1], nd[1]);
    gen_wall(&g, 0, x0 + t / 2, z0 + t, z1 - t, c[1], s[1], t, doors[2], nd[2]);
    gen_wall(&g, 0, x1 - t / 2, z0 + t, z1 - t, c[1], s[1], t, doors[3], nd[3]);
    cJSON *o = cJSON_CreateObject();
    const double inner_min[3] = {x0 + t, c[1], z0 + t}, inner_max[3] = {x1 - t, c[1] + s[1], z1 - t};
    cJSON *in = cJSON_AddObjectToObject(o, "interior");
    cJSON_AddItemToObject(in, "min", js_vec3_new(inner_min));
    cJSON_AddItemToObject(in, "max", js_vec3_new(inner_max));
    return gen_end(&g, r, o);
}

static int dir_vector(const cJSON *args, double *dx, double *dz, int *ramp_d) {
    const char *dir = js_str(args, "direction", "+x");
    if (!strcmp(dir, "+x")) { *dx = 1; *dz = 0; *ramp_d = 0; }
    else if (!strcmp(dir, "-x")) { *dx = -1; *dz = 0; *ramp_d = 2; }
    else if (!strcmp(dir, "+z")) { *dx = 0; *dz = 1; *ramp_d = 1; }
    else if (!strcmp(dir, "-z")) { *dx = 0; *dz = -1; *ramp_d = 3; }
    else return -1;
    return 0;
}

static int t_stairs(Ctx *ctx, const cJSON *args, ToolResult *r) {
    double start[3] = {0, 0, 0};
    js_vec3(js_get(args, "start"), start);
    double dx, dz;
    int ramp_d;
    if (dir_vector(args, &dx, &dz, &ramp_d) != 0) return tr_error(r, "direction is +x, -x, +z or -z");
    const double rise = js_num(args, "height", 20);
    const double width = js_num(args, "width", 10);
    const char *style = tool_str(args, "style", "steps");
    const double climb = game_constants.climb_height;
    double step_h = js_num(args, "step_height", fmin(climb, 2.5));
    if (step_h > climb && !js_bool(args, "force", 0)) return tr_error(r, "step_height %.2f is above the player's climb height %.2f (pass force:true)", step_h, climb);
    if (rise <= 0 || width <= 0) return tr_error(r, "height and width must be > 0");
    int steps = js_int(args, "steps", (int) ceil(rise / step_h));
    if (steps < 1) steps = 1;
    if (steps > 200) return tr_error(r, "too many steps");
    step_h = rise / steps;
    const double depth = js_num(args, "step_depth", 4);
    Gen g;
    if (gen_begin(ctx, args, r, &g, "generate stairs") != 0) return 0;
    const double run = depth * steps;
    if (!strcmp(style, "ramp")) {
        const double c[3] = {start[0] + dx * run / 2, start[1], start[2] + dz * run / 2};
        const double sz[3] = {dx ? run : width, rise, dz ? run : width};
        cJSON *extra = cJSON_CreateObject();
        cJSON_AddNumberToObject(extra, "direction", ramp_d);
        gen_box(&g, "RAMP", c, sz, extra);
        cJSON_Delete(extra);
    } else {
        for (int i = 0; i < steps; i++) {
            const double along = depth * i + depth / 2;
            const double c[3] = {start[0] + dx * along, start[1], start[2] + dz * along};
            const double sz[3] = {dx ? depth : width, step_h * (i + 1), dz ? depth : width};
            gen_box(&g, "CUBE", c, sz, NULL);
        }
    }
    cJSON *o = cJSON_CreateObject();
    const double top[3] = {start[0] + dx * run, start[1] + rise, start[2] + dz * run};
    cJSON_AddItemToObject(o, "top", js_vec3_new(top));
    cJSON_AddNumberToObject(o, "step_height", js_round(step_h, 3));
    cJSON_AddNumberToObject(o, "steps", !strcmp(style, "ramp") ? 1 : steps);
    return gen_end(&g, r, o);
}

static int t_platform_path(Ctx *ctx, const cJSON *args, ToolResult *r) {
    const cJSON *pts = js_get(args, "points");
    double size[3] = {8, 2, 8};
    js_vec3(js_get(args, "size"), size);
    const double jitter = js_num(args, "jitter", 0);
    pcg32_random_t rng;
    pcg32_srandom_r(&rng, (uint64_t) js_int(args, "seed", 7), 11u);
    // polyline: points, or start -> end with `count` platforms
    int np = 0;
    double (*p)[3] = NULL;
    if (cJSON_IsArray(pts) && cJSON_GetArraySize(pts) >= 2) {
        np = cJSON_GetArraySize(pts);
        p = calloc((size_t) np, sizeof(*p));
        for (int i = 0; i < np; i++) if (!js_vec3(cJSON_GetArrayItem(pts, i), p[i])) { free(p); return tr_error(r, "points[%d] needs [x,y,z]", i); }
    } else {
        double a[3], b[3];
        if (!js_vec3(js_get(args, "start"), a) || !js_vec3(js_get(args, "end"), b)) return tr_error(r, "generate_platform_path needs points or start + end");
        np = js_int(args, "count", 8);
        if (np < 2) np = 2;
        if (np > 500) return tr_error(r, "too many platforms");
        p = calloc((size_t) np, sizeof(*p));
        for (int i = 0; i < np; i++) for (int k = 0; k < 3; k++) p[i][k] = a[k] + (b[k] - a[k]) * i / (np - 1);
    }
    for (int i = 1; jitter > 0 && i < np - 1; i++) {
        p[i][0] += (ldexp((double) pcg32_random_r(&rng), -32) * 2 - 1) * jitter;
        p[i][2] += (ldexp((double) pcg32_random_r(&rng), -32) * 2 - 1) * jitter;
    }
    Gen g;
    if (gen_begin(ctx, args, r, &g, "generate platform path") != 0) {
        free(p);
        return 0;
    }
    const double max_gap = js_num(args, "max_gap", 14), max_rise = js_num(args, "max_rise", 8);
    cJSON *jumps = cJSON_CreateArray();
    int hard = 0;
    for (int i = 0; i < np; i++) {
        const double c[3] = {p[i][0], p[i][1] - size[1], p[i][2]};
        gen_box(&g, "CUBE", c, size, NULL);
        if (i) {
            const double horiz = hypot(p[i][0] - p[i - 1][0], p[i][2] - p[i - 1][2]);
            const double gap = fmax(0, horiz - fmax(size[0], size[2]));
            const double rise = p[i][1] - p[i - 1][1];
            const int ok = gap <= max_gap && rise <= max_rise;
            hard += !ok;
            cJSON *j = cJSON_CreateObject();
            cJSON_AddNumberToObject(j, "from", i - 1);
            cJSON_AddNumberToObject(j, "gap", js_round(gap, 2));
            cJSON_AddNumberToObject(j, "rise", js_round(rise, 2));
            cJSON_AddBoolToObject(j, "jumpable", ok);
            cJSON_AddItemToArray(jumps, j);
        }
    }
    free(p);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "jumps", jumps);
    cJSON_AddNumberToObject(o, "too_hard", hard);
    cJSON_AddStringToObject(o, "note", "platform tops sit at the given points; gap = edge-to-edge distance");
    return gen_end(&g, r, o);
}

static int t_fill_area(Ctx *ctx, const cJSON *args, ToolResult *r) {
    double mn[3], mx[3];
    const cJSON *bbox = js_get(args, "bbox");
    if (!js_vec3(js_get(bbox, "min"), mn) || !js_vec3(js_get(bbox, "max"), mx)) return tr_error(r, "fill_area needs bbox {min:[x,y,z], max:[x,y,z]}");
    const char *prefab = tool_str(args, "prefab", "CRATE");
    const int pid = schema_prefab_find(prefab);
    if (pid < 0) return tr_error(r, "unknown prefab %s", prefab);
    const SchemaPrefab *pf = schema_prefab(pid);
    int count = js_int(args, "count", 0);
    const double spacing = js_num(args, "spacing", 0);
    if (!count && spacing <= 0) count = 10;
    if (count > 2000) return tr_error(r, "count is limited to 2000");
    double scale[2] = {1, 1};
    const cJSON *sr = js_get(args, "scale_range");
    if (cJSON_GetArraySize(sr) == 2) {
        scale[0] = cJSON_GetArrayItem(sr, 0)->valuedouble;
        scale[1] = cJSON_GetArrayItem(sr, 1)->valuedouble;
    }
    const int on_ground = js_bool(args, "on_ground", 1), avoid = js_bool(args, "avoid_overlap", 1);
    const double avoid_spawns = js_num(args, "avoid_spawns", 12);
    pcg32_random_t rng;
    pcg32_srandom_r(&rng, (uint64_t) js_int(args, "seed", 1), 3u);
    Gen g;
    if (gen_begin(ctx, args, r, &g, "fill area") != 0) return 0;
    NativeMap nm;
    char err[256];
    const int native = native_map_load(g.p->doc, &nm, err, sizeof(err)) == 0;
    // candidate points
    int cap = count ? count * 8 : 4096, nc = 0;
    double (*cand)[2] = malloc(sizeof(*cand) * (size_t) cap);
    if (spacing > 0) {
        for (double x = mn[0] + spacing / 2; x <= mx[0] && nc < cap; x += spacing)
            for (double z = mn[2] + spacing / 2; z <= mx[2] && nc < cap; z += spacing) {
                cand[nc][0] = x + (ldexp((double) pcg32_random_r(&rng), -32) - 0.5) * js_num(args, "jitter", 0);
                cand[nc][1] = z + (ldexp((double) pcg32_random_r(&rng), -32) - 0.5) * js_num(args, "jitter", 0);
                nc++;
            }
    } else {
        for (; nc < cap; nc++) {
            cand[nc][0] = mn[0] + ldexp((double) pcg32_random_r(&rng), -32) * (mx[0] - mn[0]);
            cand[nc][1] = mn[2] + ldexp((double) pcg32_random_r(&rng), -32) * (mx[2] - mn[2]);
        }
    }
    double (*placed)[4] = malloc(sizeof(*placed) * (size_t) (cap ? cap : 1)); // x, z, half extent, top
    int np = 0, want = count ? count : nc;
    cJSON *spawns = doc_map_form(g.p->doc);
    for (int i = 0; i < nc && np < want && !g.failed; i++) {
        const double sc = scale[0] + ldexp((double) pcg32_random_r(&rng), -32) * (scale[1] - scale[0]);
        double s[3] = {pf->default_size[0] * sc, pf->default_size[1] * sc, pf->default_size[2] * sc};
        const double half = fmax(s[0], s[2]) / 2;
        const double x = cand[i][0], z = cand[i][1];
        if (x - half < mn[0] || x + half > mx[0] || z - half < mn[2] || z + half > mx[2]) continue;
        int bad = 0;
        for (int k = 0; avoid && k < np && !bad; k++) bad = fabs(placed[k][0] - x) < placed[k][2] + half && fabs(placed[k][1] - z) < placed[k][2] + half;
        const cJSON *sp;
        cJSON_ArrayForEach(sp, js_get(spawns, "spawns")) {
            double q[3];
            if (js_vec3(sp, q) && hypot(q[0] - x, q[2] - z) < avoid_spawns + half) bad = 1;
        }
        if (bad) continue;
        double y = mn[1];
        if (on_ground && native) {
            const double probe[3] = {x, mx[1], z};
            const double gy = native_ground(&nm, probe);
            if (gy < mn[1] - 0.01 || gy < -1e29) continue; // nothing to stand on inside the box
            y = gy;
        }
        const double c[3] = {x, y, z};
        cJSON *extra = cJSON_CreateObject();
        if (js_bool(args, "random_yaw", 0) && !schema_prefab_has_flag(pid, "texturable")) {
            const double ry = ldexp((double) pcg32_random_r(&rng), -32) * 2 * M_PI;
            const double rot[3] = {0, ry, 0};
            cJSON_AddItemToObject(extra, "rotation", js_vec3_new(rot));
        }
        if (fabs(sc - 1) > 1e-9 && schema_prefab_has_flag(pid, "gen")) cJSON_AddNumberToObject(extra, "model_size", js_round(pf->scale * sc, 2));
        gen_box(&g, prefab, c, s, extra);
        cJSON_Delete(extra);
        placed[np][0] = x;
        placed[np][1] = z;
        placed[np][2] = half;
        placed[np][3] = y + s[1];
        np++;
    }
    cJSON_Delete(spawns);
    free(cand);
    free(placed);
    if (native) native_map_free(&nm);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "requested", want);
    cJSON_AddNumberToObject(o, "placed", np);
    return gen_end(&g, r, o);
}

#define STYLE_ARG "props":{"type":"object","description":"properties applied to every generated object (texture, color, ...)"}

const ToolDef g_tools_build[] = {
    {"group_create", "Create group", "Name a set of objects (session group of the project) for group_stamp / prefab_save.",
     S({"type":"object","properties":{PROJECT_ARG,"name":{"type":"string"},"ids":{"type":"array","items":{"type":"integer"}}},"required":["name","ids"]}),
     0, 0, 1, 0, t_group_create},
    {"group_list", "List groups", "Session groups of a project with their (still existing) object ids.",
     S({"type":"object","properties":{PROJECT_ARG}}), 1, 0, 1, 0, t_group_list},
    {"group_delete", "Ungroup", "Remove a group (objects kept unless delete_objects:true).",
     S({"type":"object","properties":{PROJECT_ARG,"name":{"type":"string"},"delete_objects":{"type":"boolean"}},"required":["name"]}),
     0, 1, 0, 0, t_group_delete},
    {"group_stamp", "Stamp group copies",
     "Copy a group (name) or ids: the selection's bottom centre goes to `at` (default: beside it, or offset), each further copy "
     "shifted by step, rotated rotate_y_deg around its anchor (quarter turns swap footprints and turn directions). count copies, "
     "one undo step; new_group names the copies.",
     S({"type":"object","properties":{PROJECT_ARG,"name":{"type":"string"},"ids":{"type":"array","items":{"type":"integer"}},"at":{"type":"array"},"offset":{"type":"array"},"step":{"type":"array"},"count":{"type":"integer"},"rotate_y_deg":{"type":"number"},"new_group":{"type":"string"}}}),
     0, 0, 0, 0, t_group_stamp},
    {"prefab_save", "Save to prefab library",
     "Save objects (ids or group) as a reusable library prefab in the workspace (positions relative to their bottom centre), for "
     "prefab_stamp in any project.",
     S({"type":"object","properties":{PROJECT_ARG,"name":{"type":"string"},"ids":{"type":"array","items":{"type":"integer"}},"group":{"type":"string"},"description":{"type":"string"}},"required":["name"]}),
     0, 0, 1, 0, t_prefab_save},
    {"prefab_list", "List library prefabs", "Library prefabs saved with prefab_save.", S({"type":"object","properties":{}}), 1, 0, 1, 0, t_prefab_list},
    {"prefab_stamp", "Stamp library prefab", "Place a library prefab at `at` (its bottom centre), count copies along step, rotate_y_deg.",
     S({"type":"object","properties":{PROJECT_ARG,"name":{"type":"string"},"at":{"type":"array"},"step":{"type":"array"},"count":{"type":"integer"},"rotate_y_deg":{"type":"number"}},"required":["name"]}),
     0, 0, 0, 0, t_prefab_stamp},
    {"generate_box_room", "Generate box room",
     "A walled room: center = bottom centre of the interior floor level, size [w, h, d], wall thickness, floor (default true), "
     "ceiling, doors [{side: north (-Z) | south (+Z) | west (-X) | east (+X), offset along the wall, width, height}]. Walls are "
     "split around doors with lintels. props style every piece (texture, color); floor_props the floor.",
     S({"type":"object","properties":{PROJECT_ARG,"center":{"type":"array"},"size":{"type":"array"},"wall":{"type":"number","default":2},"floor":{"type":"boolean"},"ceiling":{"type":"boolean"},"doors":{"type":"array","items":{"type":"object","properties":{"side":{"type":"string","enum":["north","south","west","east"]},"offset":{"type":"number"},"width":{"type":"number"},"height":{"type":"number"}}}},STYLE_ARG,"floor_props":{"type":"object"},"force":{"type":"boolean"}}}),
     0, 0, 0, 0, t_box_room},
    {"generate_stairs", "Generate stairs",
     "Stairs from start (bottom front centre) going direction (+x, -x, +z, -z) up `height`: style steps (solid blocks, step "
     "height <= the player's climb height) or ramp (one RAMP prefab). width, step_depth, steps / step_height.",
     S({"type":"object","properties":{PROJECT_ARG,"start":{"type":"array"},"direction":{"type":"string","enum":["+x","-x","+z","-z"]},"height":{"type":"number"},"width":{"type":"number"},"step_height":{"type":"number"},"step_depth":{"type":"number"},"steps":{"type":"integer"},"style":{"type":"string","enum":["steps","ramp"]},STYLE_ARG,"force":{"type":"boolean"}}}),
     0, 0, 0, 0, t_stairs},
    {"generate_platform_path", "Generate platform path",
     "Parkour platforms whose tops sit on points [[x,y,z],...] or on `count` points from start to end (jitter / seed shift the "
     "middle ones). size of each platform; reports every jump's edge gap and rise against max_gap / max_rise.",
     S({"type":"object","properties":{PROJECT_ARG,"points":{"type":"array"},"start":{"type":"array"},"end":{"type":"array"},"count":{"type":"integer"},"size":{"type":"array"},"jitter":{"type":"number"},"seed":{"type":"integer"},"max_gap":{"type":"number"},"max_rise":{"type":"number"},STYLE_ARG}}),
     0, 0, 0, 0, t_platform_path},
    {"fill_area", "Fill area",
     "Scatter a prefab (default CRATE) inside bbox: count random spots or a spacing grid (+jitter), scale_range [a, b], "
     "random_yaw, on_ground (stand on the highest solid inside the box, default), avoid_overlap, avoid_spawns radius, seed.",
     S({"type":"object","properties":{PROJECT_ARG,"bbox":{"type":"object","properties":{"min":{"type":"array"},"max":{"type":"array"}}},"prefab":{"type":["string","integer"]},"count":{"type":"integer"},"spacing":{"type":"number"},"jitter":{"type":"number"},"scale_range":{"type":"array"},"random_yaw":{"type":"boolean"},"on_ground":{"type":"boolean"},"avoid_overlap":{"type":"boolean"},"avoid_spawns":{"type":"number"},"seed":{"type":"integer"},STYLE_ARG},"required":["bbox"]}),
     0, 0, 0, 0, t_fill_area},
    {0},
};
