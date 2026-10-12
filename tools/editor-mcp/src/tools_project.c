// Projects, map files, settings, history and logs.
#define _GNU_SOURCE
#include "jsonw.h"
#include "mcp.h"
#include "schema.h"
#include "util.h"
#include <map_catalog.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- helpers

static int resolve_map_path(Ctx *ctx, const char *ref, char *out, const size_t size, char *err, const size_t errsz) {
    const size_t n = strlen(ref);
    const int looks_like_path = strchr(ref, '/') || (n > 5 && !strcasecmp(ref + n - 5, ".json"));
    if (!looks_like_path) {
        int cands[8];
        size_t cand_count = 0;
        const int idx = map_catalog_resolve(ref, cands, 8, &cand_count);
        if (idx >= 0) {
            const MapCatalogEntry *e = map_catalog_at((size_t) idx);
            char rel[PATH_MAX];
            snprintf(rel, sizeof(rel), "assets/%s", e->path); // the catalogue's file (maps/v7/, maps/custom/, v3 maps/)
            return ctx_safe_path(ctx, rel, 0, out, size, err, errsz);
        }
        if (idx == -2) {
            Sb sb = {0};
            for (size_t i = 0; i < cand_count && i < 8; i++) sb_printf(&sb, "%s%s", i ? ", " : "", map_catalog_at((size_t) cands[i])->key);
            char *list = sb_take(&sb);
            snprintf(err, errsz, "'%s' is ambiguous: %s", ref, list ? list : "");
            free(list);
            return -1;
        }
        // a map in the workspace
        char ws[PATH_MAX];
        snprintf(ws, sizeof(ws), "%s/%s.json", ctx->workspace, ref);
        if (file_exists(ws)) return ctx_safe_path(ctx, ws, 0, out, size, err, errsz);
        snprintf(err, errsz, "no catalog map or workspace map named '%s' (krunker://catalog lists them)", ref);
        return -1;
    }
    return ctx_safe_path(ctx, ref, 0, out, size, err, errsz);
}

static int doc_json_size(const Doc *doc) {
    char *t = jsonw_print(doc->root, JSONW_COMPACT);
    const int n = t ? (int) strlen(t) : 0;
    free(t);
    return n;
}

static JsonwStyle style_for(const Doc *doc, const char *format) {
    if (format && !strcmp(format, "pretty")) return JSONW_PRETTY;
    if (format && !strcmp(format, "indent2")) return JSONW_INDENT2;
    if (format && !strcmp(format, "compact")) return JSONW_COMPACT;
    return doc->source_pretty ? JSONW_INDENT2 : JSONW_COMPACT;
}

static int write_doc(const Doc *doc, const char *path, const char *format, size_t *bytes, char *err, const size_t errsz) {
    const JsonwStyle style = style_for(doc, format);
    char *text = jsonw_print(doc->root, style);
    if (!text) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    size_t n = strlen(text);
    int newline = (format == NULL || !strcmp(format, "source")) ? doc->trailing_newline : style != JSONW_COMPACT;
    if (newline) {
        char *t = realloc(text, n + 2);
        if (t) {
            text = t;
            text[n++] = '\n';
            text[n] = 0;
        }
    }
    const int rc = file_write(path, text, n);
    free(text);
    if (rc != 0) {
        snprintf(err, errsz, "cannot write %s", path);
        return -1;
    }
    if (bytes) *bytes = n;
    return 0;
}

// ---------------------------------------------------------------- projects

static int t_projects_list(Ctx *ctx, const cJSON *args, ToolResult *r) {
    (void) args;
    char err[512] = "";
    int failed = 0;
    if (ctx->live.mode == 2 || (ctx->live.mode && live_bus_present(&ctx->live))) failed = projects_refresh_live(ctx, err, sizeof(err)) != 0;
    if (ctx->live.mode == 2 && !live_bus_present(&ctx->live) && !failed) {
        failed = 1;
        snprintf(err, sizeof(err), "--live: no editor bus found (start one with client_bin --editor --mcp-bus)");
    }
    // --live: the editor is required, so a refused / missing bus is the tool's failure
    if (failed && ctx->live.mode == 2) return tr_error(r, "%s", err);
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "projects");
    for (int i = 0; i < ctx->project_count; i++) cJSON_AddItemToArray(arr, project_json(ctx, ctx->projects[i]));
    cJSON_AddItemToObject(o, "editor", live_status_json(ctx));
    if (err[0]) cJSON_AddStringToObject(o, "editor_error", err);
    cJSON_AddItemToObject(o, "allowed_dirs", ctx_roots_json(ctx));
    cJSON_AddStringToObject(o, "workspace", ctx->workspace);
    cJSON_AddStringToObject(o, "root", ctx->root);
    tr_json(r, o);
    return 0;
}

static int t_map_open(Ctx *ctx, const cJSON *args, ToolResult *r) {
    const char *ref = tool_str(args, "path", tool_str(args, "name", NULL));
    if (!ref) return tr_error(r, "map_open needs path (a file under the allowed dirs, or a catalog name like \"burg\")");
    char err[512], path[PATH_MAX];
    if (js_bool(args, "editor", 0)) {
        if (resolve_map_path(ctx, ref, path, sizeof(path), err, sizeof(err)) != 0) return tr_error(r, "%s", err);
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "path", path);
        cJSON *res = live_request(ctx, "open", a, 30000, err, sizeof(err));
        if (!res) return tr_error(r, "%s", err);
        cJSON_Delete(res);
        projects_refresh_live(ctx, err, sizeof(err));
        Project *p = NULL;
        if (project_resolve(ctx, path, &p, err, sizeof(err)) != 0) return tr_error(r, "opened in the editor, but: %s", err);
        ctx->focus = project_index(ctx, p);
        tr_json(r, project_json(ctx, p));
        return 0;
    }
    if (resolve_map_path(ctx, ref, path, sizeof(path), err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    for (int i = 0; i < ctx->project_count; i++) {
        if (ctx->projects[i]->kind == PROJ_FILE && !strcmp(ctx->projects[i]->path, path)) {
            if (!js_bool(args, "reload", 0)) {
                ctx->focus = i;
                cJSON *o = project_json(ctx, ctx->projects[i]);
                cJSON_AddTrueToObject(o, "already_open");
                tr_json(r, o);
                return 0;
            }
            project_remove(ctx, ctx->projects[i]);
            break;
        }
    }
    size_t n;
    char *text = file_read(path, &n);
    if (!text) return tr_error(r, "cannot read %s", path);
    Doc *doc = doc_from_text(text, n, err, sizeof(err));
    free(text);
    if (!doc) return tr_error(r, "%s: %s", path, err);
    snprintf(doc->path, sizeof(doc->path), "%s", path);
    Project *p = project_add_file(ctx, doc, path, NULL);
    if (!p) {
        doc_free(doc);
        return tr_error(r, "out of memory");
    }
    if (js_bool(args, "focus", 1) == 0 && ctx->project_count > 1) ctx->focus = ctx->project_count - 2;
    log_msg(LOG_INFO, "mcp", "opened %s as %s (%d objects)", path, p->id, doc_object_count(doc));
    tr_json(r, project_json(ctx, p));
    return 0;
}

static int t_map_new(Ctx *ctx, const cJSON *args, ToolResult *r) {
    const char *name = tool_str(args, "name", "New Krunker Map");
    const char *tmpl = tool_str(args, "template", "blank");
    char err[512];
    if (js_bool(args, "editor", 0)) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", name);
        cJSON *res = live_request(ctx, "new", a, 30000, err, sizeof(err));
        if (!res) return tr_error(r, "%s", err);
        projects_refresh_live(ctx, err, sizeof(err));
        tr_json(r, res);
        return 0;
    }
    Doc *doc = doc_from_template(tmpl, name);
    if (!doc) return tr_error(r, "unknown template '%s' (blank, empty, arena)", tmpl);
    char path[PATH_MAX] = "";
    const char *want = tool_str(args, "path", NULL);
    if (want) {
        if (ctx_safe_path(ctx, want, 1, path, sizeof(path), err, sizeof(err)) != 0) {
            doc_free(doc);
            return tr_error(r, "%s", err);
        }
        if (file_exists(path) && !js_bool(args, "overwrite", 0)) {
            doc_free(doc);
            return tr_error(r, "%s exists (pass overwrite:true)", path);
        }
        snprintf(doc->path, sizeof(doc->path), "%s", path);
    }
    Project *p = project_add_file(ctx, doc, path[0] ? path : NULL, name);
    if (!p) {
        doc_free(doc);
        return tr_error(r, "out of memory");
    }
    tr_json(r, project_json(ctx, p));
    return 0;
}

static int t_map_save(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    if (ctx->read_only) return tr_error(r, "the server runs --read-only");
    const char *want = tool_str(args, "path", NULL);
    const int as = js_bool(args, "as", 0);
    char err[512], path[PATH_MAX] = "";
    if (want && ctx_safe_path(ctx, want, 1, path, sizeof(path), err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    if (p->kind == PROJ_LIVE) {
        cJSON *a = cJSON_CreateObject();
        if (path[0]) cJSON_AddStringToObject(a, "path", path);
        if (as) cJSON_AddTrueToObject(a, "as");
        cJSON *res = live_call(ctx, p->id, "save", a, 30000, err, sizeof(err));
        if (!res) return tr_error(r, "%s", err);
        tr_json(r, res);
        return 0;
    }
    if (!path[0]) {
        if (!p->doc->path[0]) return tr_error(r, "this map was never saved: pass path (e.g. \"%s/%s.json\")", ctx->workspace, "my_map");
        snprintf(path, sizeof(path), "%s", p->doc->path);
        if (ctx_safe_path(ctx, path, 1, path, sizeof(path), err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    } else if (strcmp(path, p->doc->path) && file_exists(path) && !js_bool(args, "overwrite", 0)) {
        return tr_error(r, "%s exists (pass overwrite:true)", path);
    }
    size_t bytes = 0;
    if (write_doc(p->doc, path, tool_str(args, "format", NULL), &bytes, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    const int retarget = as || !p->doc->path[0] || !strcmp(path, p->doc->path);
    if (retarget) {
        snprintf(p->doc->path, sizeof(p->doc->path), "%s", path);
        snprintf(p->path, sizeof(p->path), "%s", path);
        p->doc->dirty = 0;
    }
    log_msg(LOG_INFO, "mcp", "saved %s (%zu bytes)", path, bytes);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "path", path);
    cJSON_AddNumberToObject(o, "bytes", (double) bytes);
    cJSON_AddBoolToObject(o, "project_path_changed", retarget);
    tr_json(r, o);
    return 0;
}

static int t_map_save_as(Ctx *ctx, const cJSON *args, ToolResult *r) {
    if (!tool_str(args, "path", NULL)) return tr_error(r, "map_save_as needs path");
    cJSON *a = cJSON_Duplicate(args, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(a, "as");
    cJSON_AddTrueToObject(a, "as");
    const int rc = t_map_save(ctx, a, r);
    cJSON_Delete(a);
    return rc;
}

static int t_project_close(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    char err[512];
    if (p->kind == PROJ_LIVE) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "project", p->id);
        if (js_bool(args, "force", 0)) cJSON_AddTrueToObject(a, "force");
        cJSON *res = live_request(ctx, "close", a, 30000, err, sizeof(err));
        if (!res) return tr_error(r, "%s", err);
        cJSON_Delete(res);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "closed", p->id);
        projects_refresh_live(ctx, err, sizeof(err));
        tr_json(r, o);
        return 0;
    }
    if (p->doc && p->doc->dirty && !js_bool(args, "force", 0))
        return tr_error(r, "%s has unsaved changes: map_save it or pass force:true", p->id);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "closed", p->id);
    project_remove(ctx, p);
    cJSON_AddNumberToObject(o, "open_projects", ctx->project_count);
    tr_json(r, o);
    return 0;
}

static int t_project_focus(Ctx *ctx, const cJSON *args, ToolResult *r) {
    if (!tool_str(args, "project", NULL)) return tr_error(r, "project_focus needs project");
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    ctx->focus = project_index(ctx, p);
    if (p->kind == PROJ_LIVE && (ctx->live.granted & EDBUS_SCOPE_PROJECT)) {
        char err[256];
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "project", p->id);
        cJSON_Delete(live_request(ctx, "focus", a, 10000, err, sizeof(err)));
    }
    tr_json(r, project_json(ctx, p));
    return 0;
}

static int t_editor_connect(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Live *l = &ctx->live;
    if (l->mode == 0) return tr_error(r, "live editor mode is off (--no-live)");
    const cJSON *scopes = js_get(args, "scopes");
    if (cJSON_IsArray(scopes)) {
        const unsigned s = edbus_scope_from_json(scopes);
        if (s) l->want_scopes = s;
        live_disconnect(l);
    }
    if (js_bool(args, "retry", 0)) {
        l->denied = 0;
        l->granted = 0;
        live_disconnect(l);
    }
    char err[512];
    if (live_ensure(ctx, EDBUS_SCOPE_READ, err, sizeof(err)) != 0) {
        cJSON *o = live_status_json(ctx);
        cJSON_AddStringToObject(o, "error", err);
        tr_json(r, o);
        r->is_error = 1;
        return 0;
    }
    projects_refresh_live(ctx, err, sizeof(err));
    cJSON *o = live_status_json(ctx);
    cJSON *arr = cJSON_AddArrayToObject(o, "projects");
    for (int i = 0; i < ctx->project_count; i++)
        if (ctx->projects[i]->kind == PROJ_LIVE) cJSON_AddItemToArray(arr, project_json(ctx, ctx->projects[i]));
    tr_json(r, o);
    return 0;
}

// ---------------------------------------------------------------- info / settings

cJSON *mcp_map_info(Ctx *ctx, Project *p) {
    Doc *doc = p->doc;
    cJSON *o = project_json(ctx, p);
    cJSON *objects = doc_objects(doc);
    const int count = cJSON_GetArraySize(objects);
    int collidable = 0, triggers = 0;
    int *per_prefab = calloc((size_t) schema_prefab_count() + 1, sizeof(int));
    double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
    cJSON *cats = cJSON_CreateObject();
    cJSON *iface = cJSON_CreateObject();
    for (const cJSON *ob = objects->child; ob; ob = ob->next) {
        const int prefab = obj_prefab(ob);
        if (per_prefab) per_prefab[prefab]++;
        const SchemaPrefab *pf = schema_prefab(prefab);
        if (obj_collidable(ob) && !schema_prefab_has_flag(prefab, "noCol")) collidable++;
        if (pf && !strcmp(pf->name, "TRIGGER")) triggers++;
        const char *cat = pf ? pf->category : "props";
        cJSON *c = cJSON_GetObjectItemCaseSensitive(cats, cat);
        if (c) cJSON_SetNumberValue(c, c->valuedouble + 1);
        else cJSON_AddNumberToObject(cats, cat, 1);
        const int in = js_int(ob, "in", 0);
        if (in) {
            char k[16];
            snprintf(k, sizeof(k), "%d", in);
            cJSON *e = cJSON_GetObjectItemCaseSensitive(iface, k);
            if (e) cJSON_SetNumberValue(e, e->valuedouble + 1);
            else cJSON_AddNumberToObject(iface, k, 1);
        }
        double a[3], b[3];
        obj_bounds(doc, ob, a, b);
        for (int k = 0; k < 3; k++) {
            if (a[k] < mn[k]) mn[k] = a[k];
            if (b[k] > mx[k]) mx[k] = b[k];
        }
    }
    cJSON_AddNumberToObject(o, "object_count", count);
    cJSON_AddNumberToObject(o, "collidable_count", collidable);
    cJSON_AddItemToObject(o, "categories", cats);
    cJSON *top = cJSON_AddArrayToObject(o, "prefabs");
    for (int pass = 0; per_prefab && pass < 24; pass++) {
        int best = -1;
        for (int i = 0; i < schema_prefab_count(); i++) if (per_prefab[i] > 0 && (best < 0 || per_prefab[i] > per_prefab[best])) best = i;
        if (best < 0) break;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "prefab", schema_prefab(best)->name);
        cJSON_AddNumberToObject(e, "count", per_prefab[best]);
        cJSON_AddItemToArray(top, e);
        per_prefab[best] = 0;
    }
    free(per_prefab);
    if (count) {
        cJSON *b = cJSON_AddObjectToObject(o, "bounds");
        cJSON_AddItemToObject(b, "min", js_vec3_new(mn));
        cJSON_AddItemToObject(b, "max", js_vec3_new(mx));
        const double size[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
        cJSON_AddItemToObject(b, "size", js_vec3_new(size));
    }
    // spawns
    cJSON *m = doc_map_form(doc);
    const cJSON *spawns = js_get(m, "spawns");
    cJSON *sp = cJSON_AddObjectToObject(o, "spawns");
    cJSON_AddNumberToObject(sp, "count", cJSON_GetArraySize(spawns));
    int teams[3] = {0};
    const cJSON *s;
    cJSON_ArrayForEach(s, spawns) {
        const int t = cJSON_IsNumber(cJSON_GetArrayItem(s, 3)) ? (int) cJSON_GetArrayItem(s, 3)->valuedouble : 0;
        teams[t >= 0 && t <= 2 ? t : 0]++;
    }
    cJSON_AddNumberToObject(sp, "any_team", teams[0]);
    cJSON_AddNumberToObject(sp, "team1", teams[1]);
    cJSON_AddNumberToObject(sp, "team2", teams[2]);
    cJSON_AddNumberToObject(o, "triggers", triggers);
    cJSON_AddItemToObject(o, "interfaces", iface);
    const cJSON *config = js_get(doc->root, "config");
    const cJSON *modes = js_get(config, "modes");
    if (!modes) modes = js_get(doc->root, "modes");
    cJSON *mode_names = cJSON_AddArrayToObject(o, "modes");
    const cJSON *md;
    cJSON_ArrayForEach(md, modes) {
        const cJSON *e = cJSON_GetArrayItem(js_get(schema_json(), "modes"), cJSON_IsNumber(md) ? (int) md->valuedouble : -1);
        cJSON_AddItemToArray(mode_names, cJSON_CreateString(js_str(e, "name", "?")));
    }
    cJSON *settings = cJSON_AddObjectToObject(o, "settings");
    static const char *keys[] = {"sky", "fog", "fogD", "ambient", "ambientI", "light", "lightI", "dthY", "shadowR", "shadowD", NULL};
    for (int i = 0; keys[i]; i++) {
        const cJSON *v = js_get(doc->root, keys[i]);
        if (v) cJSON_AddItemToObject(settings, keys[i], cJSON_Duplicate(v, 1));
    }
    cJSON *unknown = cJSON_CreateArray();
    for (const cJSON *k = doc->root->child; k; k = k->next) if (!schema_map_key(k->string)) cJSON_AddItemToArray(unknown, cJSON_CreateString(k->string));
    if (cJSON_GetArraySize(unknown)) cJSON_AddItemToObject(o, "unknown_keys", unknown);
    else cJSON_Delete(unknown);
    cJSON_AddBoolToObject(o, "has_scripts", js_get(doc->root, "scripts") != NULL);
    const int size = doc_json_size(doc);
    cJSON *lim = cJSON_AddObjectToObject(o, "limits");
    cJSON_AddNumberToObject(lim, "json_chars", size);
    cJSON_AddNumberToObject(lim, "publish_max_chars", schema_limit("publishMaxChars", 500000));
    cJSON_AddNumberToObject(lim, "object_limit", schema_limit("objectLimit", 5000));
    cJSON_AddNumberToObject(lim, "collidable_limit", schema_limit("objectColLimit", 4500));
    cJSON_AddNumberToObject(lim, "spawn_limit", schema_limit("spawnLimit", 20));
    cJSON_Delete(m);
    return o;
}

static int t_map_info(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    tr_json(r, mcp_map_info(ctx, p));
    return 0;
}

static int t_settings_get(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const char *group = tool_str(args, "group", NULL);
    const cJSON *keys = js_get(args, "keys");
    cJSON *o = cJSON_CreateObject();
    cJSON *values = cJSON_AddObjectToObject(o, "values");
    cJSON *schema = cJSON_AddObjectToObject(o, "schema");
    const cJSON *mk;
    cJSON_ArrayForEach(mk, js_get(schema_json(), "map_keys")) {
        const char *key = js_str(mk, "key", "");
        if (!strcmp(js_str(mk, "group", ""), "structure")) continue;
        if (group && strcmp(js_str(mk, "group", ""), group)) continue;
        if (cJSON_IsArray(keys)) {
            int want = 0;
            const cJSON *k;
            cJSON_ArrayForEach(k, keys) if (cJSON_IsString(k) && !strcmp(k->valuestring, key)) want = 1;
            if (!want) continue;
        }
        const cJSON *v = js_get(p->doc->root, key);
        if (v) cJSON_AddItemToObject(values, key, cJSON_Duplicate(v, 1));
        cJSON_AddItemToObject(schema, key, cJSON_Duplicate(mk, 1));
    }
    if (!group || !strcmp(group, "game")) {
        const cJSON *c = js_get(p->doc->root, "config");
        if (c) cJSON_AddItemToObject(values, "config", cJSON_Duplicate(c, 1));
        const cJSON *g = js_get(p->doc->root, "game");
        if (g) cJSON_AddItemToObject(values, "game", cJSON_Duplicate(g, 1));
    }
    tr_json(r, o);
    return 0;
}

static int set_map_value(Doc *doc, const char *key, const cJSON *value, const int force, char *err, const size_t errsz) {
    const cJSON *mk = schema_map_key(key);
    if (!strcmp(key, "objects") || !strcmp(key, "spawns") || !strcmp(key, "colors") || !strcmp(key, "xyz")) {
        snprintf(err, errsz, "'%s' is structural: use the object / spawn tools", key);
        return -1;
    }
    if (!mk && !force) {
        snprintf(err, errsz, "unknown map key '%s' (krunker://config lists them; force:true writes it anyway)", key);
        return -1;
    }
    const char *type = js_str(mk, "type", "any");
    cJSON *v = NULL;
    if (!value || cJSON_IsNull(value)) {
        cJSON_DeleteItemFromObjectCaseSensitive(doc->root, key);
        return 0;
    }
    if (!strcmp(type, "hex_or_int")) {
        char hex[8];
        if (!color_parse(value, hex)) {
            snprintf(err, errsz, "'%s' needs a colour \"#rrggbb\"", key);
            return -1;
        }
        const cJSON *old = js_get(doc->root, key);
        if (cJSON_IsNumber(old) || (cJSON_IsNumber(value))) v = cJSON_CreateNumber((double) strtol(hex + 1, NULL, 16));
        else v = cJSON_CreateString(hex);
    } else if (!strcmp(type, "number")) {
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
            snprintf(err, errsz, "'%s' needs a number", key);
            return -1;
        }
        double d = value->valuedouble;
        if (cJSON_IsNumber(js_get(mk, "min")) && d < js_num(mk, "min", 0)) {
            snprintf(err, errsz, "'%s' must be >= %g", key, js_num(mk, "min", 0));
            return -1;
        }
        if (cJSON_IsNumber(js_get(mk, "max")) && d > js_num(mk, "max", 0)) {
            snprintf(err, errsz, "'%s' must be <= %g", key, js_num(mk, "max", 0));
            return -1;
        }
        v = cJSON_CreateNumber(d);
    } else if (!strcmp(type, "string")) {
        if (!cJSON_IsString(value)) {
            snprintf(err, errsz, "'%s' needs a string", key);
            return -1;
        }
        v = cJSON_CreateString(value->valuestring);
    } else if (!strcmp(type, "bool")) {
        v = cJSON_CreateBool(js_truthy(value));
    } else {
        v = cJSON_Duplicate(value, 1);
    }
    if (cJSON_GetObjectItemCaseSensitive(doc->root, key)) cJSON_ReplaceItemInObjectCaseSensitive(doc->root, key, v);
    else cJSON_AddItemToObject(doc->root, key, v);
    return 0;
}

static int index_of_named(const cJSON *list, const cJSON *v) {
    if (cJSON_IsNumber(v)) return (int) v->valuedouble >= 0 && (int) v->valuedouble < cJSON_GetArraySize(list) ? (int) v->valuedouble : -1;
    if (!cJSON_IsString(v)) return -1;
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        if (!strcasecmp(js_str(it, "name", ""), v->valuestring)) return i;
        i++;
    }
    return -1;
}

static int set_config(Doc *doc, const cJSON *cfg, char *err, const size_t errsz) {
    cJSON *config = cJSON_GetObjectItemCaseSensitive(doc->root, "config");
    if (!cJSON_IsObject(config)) {
        cJSON_DeleteItemFromObjectCaseSensitive(doc->root, "config");
        config = cJSON_AddObjectToObject(doc->root, "config");
    }
    static const char *lists[2][2] = {{"modes", "modes"}, {"classes", "classes"}};
    for (int l = 0; l < 2; l++) {
        const cJSON *v = js_get(cfg, lists[l][0]);
        if (!v) continue;
        if (!cJSON_IsArray(v)) {
            snprintf(err, errsz, "config.%s needs an array of names or ids", lists[l][0]);
            return -1;
        }
        cJSON *ids = cJSON_CreateArray();
        const cJSON *it;
        cJSON_ArrayForEach(it, v) {
            const int idx = index_of_named(js_get(schema_json(), lists[l][1]), it);
            if (idx < 0) {
                cJSON_Delete(ids);
                snprintf(err, errsz, "unknown %s entry (krunker://config lists them)", lists[l][0]);
                return -1;
            }
            cJSON_AddItemToArray(ids, cJSON_CreateNumber(idx));
        }
        cJSON_DeleteItemFromObjectCaseSensitive(config, lists[l][0]);
        if (cJSON_GetArraySize(ids)) cJSON_AddItemToObject(config, lists[l][0], ids);
        else cJSON_Delete(ids);
    }
    const cJSON *settings = js_get(cfg, "settings");
    if (cJSON_IsObject(settings)) {
        cJSON *dst = cJSON_GetObjectItemCaseSensitive(config, "settings");
        if (!cJSON_IsObject(dst)) {
            cJSON_DeleteItemFromObjectCaseSensitive(config, "settings");
            dst = cJSON_AddObjectToObject(config, "settings");
        }
        for (const cJSON *s = settings->child; s; s = s->next) {
            const cJSON *def = NULL, *w;
            cJSON_ArrayForEach(w, js_get(schema_json(), "server_config")) if (!strcmp(js_str(w, "varN", ""), s->string)) def = w;
            if (!def) {
                snprintf(err, errsz, "unknown server setting '%s' (krunker://config server_config)", s->string);
                return -1;
            }
            cJSON *v;
            if (js_bool(def, "bool", 0)) v = cJSON_CreateNumber(js_truthy(s) ? 1 : 0);
            else if (js_bool(def, "input", 0)) v = cJSON_CreateString(cJSON_IsString(s) ? s->valuestring : "");
            else {
                if (!cJSON_IsNumber(s)) {
                    snprintf(err, errsz, "setting '%s' needs a number", s->string);
                    return -1;
                }
                const double lo = js_num(def, "min", -1e30), hi = js_num(def, "max", 1e30);
                if (s->valuedouble < lo || s->valuedouble > hi) {
                    snprintf(err, errsz, "setting '%s' must be within [%g, %g]", s->string, lo, hi);
                    return -1;
                }
                v = cJSON_CreateNumber(s->valuedouble);
            }
            cJSON_DeleteItemFromObjectCaseSensitive(dst, s->string);
            cJSON_AddItemToObject(dst, s->string, v);
        }
    }
    if (!cJSON_GetArraySize(config)) cJSON_DeleteItemFromObjectCaseSensitive(doc->root, "config");
    return 0;
}

static int t_settings_set(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    Edit e;
    char err[512];
    if (edit_begin(ctx, p, "map settings", &e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    const int force = js_bool(args, "force", 0);
    const cJSON *set = js_get(args, "set");
    int changed = 0;
    for (const cJSON *k = set ? set->child : NULL; k; k = k->next, changed++) {
        if (set_map_value(p->doc, k->string, k, force, err, sizeof(err)) != 0) {
            edit_abort(&e);
            return tr_error(r, "%s", err);
        }
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, js_get(args, "unset")) {
        if (cJSON_IsString(it)) {
            cJSON_DeleteItemFromObjectCaseSensitive(p->doc->root, it->valuestring);
            changed++;
        }
    }
    const cJSON *cfg = js_get(args, "config");
    if (cJSON_IsObject(cfg)) {
        if (set_config(p->doc, cfg, err, sizeof(err)) != 0) {
            edit_abort(&e);
            return tr_error(r, "%s", err);
        }
        changed++;
    }
    if (!changed) {
        edit_abort(&e);
        return tr_error(r, "nothing to change: pass set {key: value}, unset [keys] or config {modes, classes, settings}");
    }
    if (edit_commit(&e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    edit_end(&e);
    return t_settings_get(ctx, args, r);
}

// ---------------------------------------------------------------- history

static int t_undo_redo(Ctx *ctx, const cJSON *args, ToolResult *r, const int redo) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int steps = js_int(args, "steps", 1);
    if (steps < 1) steps = 1;
    char err[512];
    if (p->kind == PROJ_LIVE) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "steps", steps);
        cJSON *res = live_call(ctx, p->id, redo ? "redo" : "undo", a, 30000, err, sizeof(err));
        if (!res) return tr_error(r, "%s", err);
        p->live_revision = -1;
        project_sync(ctx, p, err, sizeof(err));
        tr_json(r, res);
        return 0;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON *labels = cJSON_AddArrayToObject(o, redo ? "redone" : "undone");
    int done = 0;
    for (; done < steps; done++) {
        char label[128];
        if (!(redo ? doc_redo(p->doc, label, sizeof(label)) : doc_undo(p->doc, label, sizeof(label)))) break;
        cJSON_AddItemToArray(labels, cJSON_CreateString(label));
    }
    project_refresh_name(p);
    cJSON_AddNumberToObject(o, redo ? "redo_steps" : "undo_steps", done);
    cJSON_AddNumberToObject(o, "undo", p->doc->undo.count);
    cJSON_AddNumberToObject(o, "redo", p->doc->redo.count);
    cJSON_AddNumberToObject(o, "objects", doc_object_count(p->doc));
    if (!done) {
        cJSON_Delete(o);
        return tr_error(r, "nothing to %s", redo ? "redo" : "undo");
    }
    tr_json(r, o);
    return 0;
}

static int t_undo(Ctx *ctx, const cJSON *args, ToolResult *r) { return t_undo_redo(ctx, args, r, 0); }
static int t_redo(Ctx *ctx, const cJSON *args, ToolResult *r) { return t_undo_redo(ctx, args, r, 1); }

static int t_history(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const int limit = js_int(args, "limit", 50);
    if (p->kind == PROJ_LIVE) {
        char err[512];
        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "limit", limit);
        cJSON *res = live_call(ctx, p->id, "history", a, 10000, err, sizeof(err));
        if (!res) return tr_error(r, "%s", err);
        tr_json(r, res);
        return 0;
    }
    tr_json(r, doc_history_json(p->doc, limit));
    return 0;
}

static int t_logs(Ctx *ctx, const cJSON *args, ToolResult *r) {
    const char *source = tool_str(args, "source", "all");
    const int level = log_level_parse(tool_str(args, "level", "debug"));
    const int tail = js_int(args, "tail", 100);
    cJSON *o = cJSON_CreateObject();
    if (!strcmp(source, "editor")) {
        char err[512];
        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "tail", tail);
        const char *project = tool_str(args, "project", NULL);
        cJSON *res = project ? live_call(ctx, project, "log", a, 10000, err, sizeof(err)) : live_request(ctx, "log", a, 10000, err, sizeof(err));
        if (!res) {
            cJSON_Delete(o);
            return tr_error(r, "%s", err);
        }
        cJSON_AddItemToObject(o, "editor", res);
    } else {
        cJSON_AddItemToObject(o, "entries", log_tail(source, level < 0 ? LOG_DEBUG : (LogLevel) level, tail));
    }
    tr_json(r, o);
    return 0;
}

static int t_prefabs(Ctx *ctx, const cJSON *args, ToolResult *r) {
    (void) ctx;
    const char *name = tool_str(args, "name", NULL);
    const char *category = tool_str(args, "category", NULL);
    const char *search = tool_str(args, "search", NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "prefabs");
    for (int i = 0; i < schema_prefab_count(); i++) {
        const SchemaPrefab *pf = schema_prefab(i);
        if (name && schema_prefab_find(name) != i) continue;
        if (category && strcasecmp(pf->category, category)) continue;
        if (search && !strcasestr(pf->name, search) && !strcasestr(pf->label, search)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", i);
        cJSON_AddStringToObject(e, "name", pf->name);
        cJSON_AddStringToObject(e, "category", pf->category);
        cJSON_AddItemToObject(e, "default_size", js_vec3_new(pf->default_size));
        cJSON_AddBoolToObject(e, "native", pf->native);
        if (name) {
            cJSON_AddItemToObject(e, "flags", cJSON_Duplicate(pf->flags, 1));
            cJSON *props = cJSON_AddArrayToObject(e, "properties");
            for (int k = 0; k < schema_prop_count(); k++) {
                const SchemaProp *p = schema_prop_at(k);
                if (schema_core_key(p->key) || !schema_prop_allowed(p, i)) continue;
                cJSON *po = cJSON_CreateObject();
                cJSON_AddStringToObject(po, "prop", p->prop);
                cJSON_AddStringToObject(po, "key", p->key);
                cJSON_AddStringToObject(po, "type", schema_prop_type_name(p->type));
                if (p->def) cJSON_AddItemToObject(po, "default", cJSON_Duplicate(p->def, 1));
                if (p->has_min) cJSON_AddNumberToObject(po, "min", p->min);
                if (p->has_max) cJSON_AddNumberToObject(po, "max", p->max);
                if (p->desc[0]) cJSON_AddStringToObject(po, "desc", p->desc);
                cJSON_AddItemToArray(props, po);
            }
        }
        cJSON_AddItemToArray(arr, e);
    }
    if (name && !cJSON_GetArraySize(arr)) {
        cJSON_Delete(o);
        return tr_error(r, "unknown prefab '%s'", name);
    }
    if (!name) {
        cJSON *tex = cJSON_AddArrayToObject(o, "textures");
        for (int i = 0; i < schema_texture_count(); i++) cJSON_AddItemToArray(tex, cJSON_CreateString(schema_texture_name(i)));
    }
    tr_json(r, o);
    return 0;
}


const ToolDef g_tools_project[] = {
    {"projects_list", "List projects",
     "Open projects (local map files and live editor projects when a KrunkNative editor hosts the bus), the focused one, the "
     "editor connection / permission state, the allowed map directories and the workspace.",
     S({"type":"object","properties":{}}), 1, 0, 1, 0, t_projects_list},
    {"map_open", "Open a map",
     "Open a map file as a project and focus it. path: a .json under the allowed dirs (relative paths are relative to the "
     "KrunkNative root), or a catalog name (\"burg\", \"Kanji\", \"old_burg\"). editor:true opens it in the live editor instead.",
     S({"type":"object","properties":{"path":{"type":"string"},"editor":{"type":"boolean"},"reload":{"type":"boolean","description":"re-read an already open file (drops unsaved changes)"},"focus":{"type":"boolean","default":true}},"required":["path"]}),
     0, 0, 1, 0, t_map_open},
    {"project_open", "Open a project", "Alias of map_open (path, editor, reload).",
     S({"type":"object","properties":{"path":{"type":"string"},"editor":{"type":"boolean"},"reload":{"type":"boolean"}},"required":["path"]}),
     0, 0, 1, 0, t_map_open},
    {"map_new", "New map",
     "Create a new map project from a template: blank (floor + 2 spawns), empty, arena (walled floor, 4 spawns per team, FFA/TDM). "
     "Optional path (a .json under the allowed dirs) sets where map_save writes. editor:true creates it in the live editor.",
     S({"type":"object","properties":{"name":{"type":"string"},"template":{"type":"string","enum":["blank","empty","arena"]},"path":{"type":"string"},"overwrite":{"type":"boolean"},"editor":{"type":"boolean"}}}),
     0, 0, 0, 0, t_map_new},
    {"map_save", "Save map",
     "Write the project to its file (or to path: a copy, unless as:true). Output is Krunker JSON in JSON.stringify form, so an "
     "unchanged map saves byte-identically; format: source (default: like the loaded file), compact, pretty (short arrays inline) "
     "or indent2. Unknown fields are preserved.",
     S({"type":"object","properties":{PROJECT_ARG,"path":{"type":"string"},"as":{"type":"boolean"},"format":{"type":"string","enum":["source","compact","pretty","indent2"]},"overwrite":{"type":"boolean"}}}),
     0, 0, 1, 0, t_map_save},
    {"map_save_as", "Save map as", "Write the project to path and make it the project's file (map_save with as:true).",
     S({"type":"object","properties":{PROJECT_ARG,"path":{"type":"string"},"format":{"type":"string","enum":["source","compact","pretty","indent2"]},"overwrite":{"type":"boolean"}},"required":["path"]}),
     0, 0, 1, 0, t_map_save_as},
    {"project_close", "Close project", "Close a project (force:true discards unsaved changes).",
     S({"type":"object","properties":{PROJECT_ARG,"force":{"type":"boolean"}}}), 0, 1, 0, 0, t_project_close},
    {"project_focus", "Focus project", "Make a project the default target of tools (and focus its editor window for live projects).",
     S({"type":"object","properties":{PROJECT_ARG},"required":["project"]}), 1, 0, 1, 0, t_project_focus},
    {"editor_connect", "Connect to the editor",
     "Attach to the live KrunkNative editor bus (connects and runs the permission handshake; the editor shows a dialog). "
     "retry:true asks again after a denial; scopes narrows the request (read, edit, project).",
     S({"type":"object","properties":{"retry":{"type":"boolean"},"scopes":{"type":"array","items":{"type":"string","enum":["read","edit","project"]}}}}),
     0, 0, 0, 0, t_editor_connect},
    {"map_info", "Map info",
     "Summary of a project: object counts by category / prefab, bounds, spawns per team, triggers, interfaces, modes, lighting, "
     "unknown top-level keys, JSON size and limits.",
     S({"type":"object","properties":{PROJECT_ARG}}), 1, 0, 1, 0, t_map_info},
    {"map_settings_get", "Get map settings",
     "Map-level settings (sky, fog, light, ambient, shadows, death height, zone, terrain, sky dome, v7 ocean / tone mapping, game "
     "config) with their schema. group: general, sky, fog, light, physics, zone, terrain, ocean, sound, game.",
     S({"type":"object","properties":{PROJECT_ARG,"group":{"type":"string"},"keys":{"type":"array","items":{"type":"string"}}}}),
     1, 0, 1, 0, t_settings_get},
    {"map_settings_set", "Set map settings",
     "Change map settings in one undo step: set {key: value} (colours as \"#rrggbb\"; keys from krunker://config), unset [keys], "
     "config {modes: [names|ids], classes: [names|ids], settings: {varN: value}} (server config, validated against its ranges).",
     S({"type":"object","properties":{PROJECT_ARG,"set":{"type":"object"},"unset":{"type":"array","items":{"type":"string"}},"config":{"type":"object","properties":{"modes":{"type":"array"},"classes":{"type":"array"},"settings":{"type":"object"}}},"force":{"type":"boolean"}}}),
     0, 0, 1, 0, t_settings_set},
    {"undo", "Undo", "Undo the last edit(s) of a project (live projects: the editor's own undo stack).",
     S({"type":"object","properties":{PROJECT_ARG,"steps":{"type":"integer","minimum":1,"default":1}}}), 0, 0, 0, 0, t_undo},
    {"redo", "Redo", "Redo undone edit(s).",
     S({"type":"object","properties":{PROJECT_ARG,"steps":{"type":"integer","minimum":1,"default":1}}}), 0, 0, 0, 0, t_redo},
    {"history", "Edit history", "Undo / redo stacks (labels, newest first).",
     S({"type":"object","properties":{PROJECT_ARG,"limit":{"type":"integer","default":50}}}), 1, 0, 1, 0, t_history},
    {"logs", "Logs",
     "Recent log entries: source all, mcp, tool, live, render (client_bin output of the last renders), playtest; or editor (the "
     "live editor's console ring).",
     S({"type":"object","properties":{"source":{"type":"string","enum":["all","mcp","tool","live","render","playtest","editor"]},"level":{"type":"string","enum":["debug","info","warning","error"]},"tail":{"type":"integer","default":100},"project":{"type":"string"}}}),
     1, 0, 1, 0, t_logs},
    {"prefabs", "Prefab schema",
     "Prefab catalog (97 prefabs: v3.1.1 ids 0-59 run natively, v7.1.4 60-96). name: one prefab with every property it accepts "
     "(friendly name, map key, type, default, range). category: geometry, props, zones, lights, logic, effects, ai, spawns, camera. "
     "Also lists textures.",
     S({"type":"object","properties":{"name":{"type":"string"},"category":{"type":"string"},"search":{"type":"string"}}}),
     1, 0, 1, 0, t_prefabs},
    {0},
};
