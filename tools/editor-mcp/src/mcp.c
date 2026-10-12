#define _GNU_SOURCE
#include "mcp.h"
#include "jsonw.h"
#include "schema.h"
#include "util.h"
#include <map_catalog.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_protocols[] = {"2025-06-18", "2025-03-26", "2024-11-05", NULL};
static int g_initialized;
static char g_protocol[32] = MCP_PROTOCOL_LATEST;
static FILE *g_out; // stdio transport: notifications go here
static Ctx *g_ctx;

// ---------------------------------------------------------------- results

static void tr_init(ToolResult *r) {
    if (!r->content) r->content = cJSON_CreateArray();
}

void tr_text(ToolResult *r, const char *fmt, ...) {
    tr_init(r);
    char small[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    char *text = small;
    if (n >= (int) sizeof(small)) {
        text = malloc((size_t) n + 1);
        if (!text) return;
        va_start(ap, fmt);
        vsnprintf(text, (size_t) n + 1, fmt, ap);
        va_end(ap);
    }
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "text");
    cJSON_AddStringToObject(c, "text", text);
    cJSON_AddItemToArray(r->content, c);
    if (text != small) free(text);
}

void tr_json(ToolResult *r, cJSON *structured) {
    tr_init(r);
    char *text = jsonw_print(structured, JSONW_COMPACT);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "text");
    cJSON_AddStringToObject(c, "text", text ? text : "{}");
    cJSON_AddItemToArray(r->content, c);
    free(text);
    cJSON_Delete(r->structured);
    r->structured = structured;
}

void tr_image(ToolResult *r, const char *base64_png, const char *mime) {
    tr_init(r);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "image");
    cJSON_AddStringToObject(c, "data", base64_png);
    cJSON_AddStringToObject(c, "mimeType", mime ? mime : "image/png");
    cJSON_AddItemToArray(r->content, c);
}

int tr_error(ToolResult *r, const char *fmt, ...) {
    tr_init(r);
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "text");
    cJSON_AddStringToObject(c, "text", buf);
    cJSON_AddItemToArray(r->content, c);
    r->is_error = 1;
    log_msg(LOG_WARN, "tool", "%s", buf);
    return 0;
}

Project *tool_project(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = NULL;
    char err[512];
    if (project_resolve(ctx, tool_str(args, "project", NULL), &p, err, sizeof(err)) != 0) {
        tr_error(r, "%s", err);
        return NULL;
    }
    return p;
}

int *tool_ids(const cJSON *arr, int *count) {
    *count = 0;
    if (cJSON_IsNumber(arr)) {
        int *ids = malloc(sizeof(int));
        if (ids) {
            ids[0] = (int) arr->valuedouble;
            *count = 1;
        }
        return ids;
    }
    if (!cJSON_IsArray(arr)) return NULL;
    const int n = cJSON_GetArraySize(arr);
    int *ids = malloc(sizeof(int) * (size_t) (n ? n : 1));
    if (!ids) return NULL;
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsNumber(it)) ids[k++] = (int) it->valuedouble;
        else if (cJSON_IsObject(it) && cJSON_IsNumber(js_get(it, "id"))) ids[k++] = js_int(it, "id", 0);
        else {
            free(ids);
            return NULL;
        }
    }
    *count = k;
    return ids;
}

const char *tool_str(const cJSON *args, const char *key, const char *def) {
    const char *s = js_str(args, key, def);
    return s && s[0] ? s : def;
}

// ---------------------------------------------------------------- registry

static const ToolDef *const k_tables[] = {g_tools_project, g_tools_objects, g_tools_logic, g_tools_build, g_tools_check, g_tools_render, g_tools_script, NULL};

static const ToolDef *find_tool(const char *name) {
    for (int t = 0; k_tables[t]; t++) {
        for (const ToolDef *d = k_tables[t]; d->name; d++) if (!strcmp(d->name, name)) return d;
    }
    return NULL;
}

static cJSON *tools_list(void) {
    cJSON *tools = cJSON_CreateArray();
    for (int t = 0; k_tables[t]; t++) {
        for (const ToolDef *d = k_tables[t]; d->name; d++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", d->name);
            if (d->title) cJSON_AddStringToObject(o, "title", d->title);
            cJSON_AddStringToObject(o, "description", d->description);
            cJSON *schema = cJSON_Parse(d->input_schema ? d->input_schema : "{\"type\":\"object\"}");
            if (!schema) {
                log_msg(LOG_ERROR, "mcp", "tool %s has an invalid input schema", d->name);
                schema = cJSON_Parse("{\"type\":\"object\"}");
            }
            cJSON_AddItemToObject(o, "inputSchema", schema);
            cJSON *ann = cJSON_AddObjectToObject(o, "annotations");
            if (d->title) cJSON_AddStringToObject(ann, "title", d->title);
            cJSON_AddBoolToObject(ann, "readOnlyHint", d->read_only);
            cJSON_AddBoolToObject(ann, "destructiveHint", d->destructive);
            cJSON_AddBoolToObject(ann, "idempotentHint", d->idempotent);
            cJSON_AddBoolToObject(ann, "openWorldHint", d->open_world);
            cJSON_AddItemToArray(tools, o);
        }
    }
    return tools;
}

cJSON *mcp_call_tool(Ctx *ctx, const char *name, const cJSON *args) {
    const ToolDef *d = find_tool(name);
    if (!d) return NULL;
    ToolResult r = {0};
    cJSON *empty = NULL;
    if (!args || cJSON_IsNull(args)) args = empty = cJSON_CreateObject();
    const double t0 = now_seconds();
    if (!cJSON_IsObject(args)) tr_error(&r, "arguments must be an object");
    else d->fn(ctx, args, &r);
    log_msg(r.is_error ? LOG_WARN : LOG_DEBUG, "mcp", "tool %s %s (%.0f ms)", name, r.is_error ? "failed" : "ok", (now_seconds() - t0) * 1000.0);
    cJSON_Delete(empty);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "content", r.content ? r.content : cJSON_CreateArray());
    if (r.structured) cJSON_AddItemToObject(res, "structuredContent", r.structured);
    cJSON_AddBoolToObject(res, "isError", r.is_error);
    return res;
}

// ---------------------------------------------------------------- resources

static char *read_root_file(const Ctx *ctx, const char *rel) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", ctx->root, rel);
    return file_read(path, NULL);
}

static cJSON *prefab_catalog(int only) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < schema_prefab_count(); i++) {
        if (only >= 0 && i != only) continue;
        const SchemaPrefab *pf = schema_prefab(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", pf->id);
        cJSON_AddStringToObject(o, "name", pf->name);
        cJSON_AddStringToObject(o, "label", pf->label);
        cJSON_AddStringToObject(o, "category", pf->category);
        cJSON_AddStringToObject(o, "since", pf->since);
        cJSON_AddBoolToObject(o, "native", pf->native);
        cJSON_AddItemToObject(o, "default_size", js_vec3_new(pf->default_size));
        if (cJSON_GetArraySize(pf->aliases)) cJSON_AddItemToObject(o, "aliases", cJSON_Duplicate(pf->aliases, 1));
        cJSON_AddItemToObject(o, "flags", cJSON_Duplicate(pf->flags, 1));
        cJSON *props = cJSON_AddArrayToObject(o, "properties");
        for (int k = 0; k < schema_prop_count(); k++) {
            const SchemaProp *p = schema_prop_at(k);
            if (schema_core_key(p->key) || !schema_prop_allowed(p, i)) continue;
            if (only < 0) {
                cJSON_AddItemToArray(props, cJSON_CreateString(p->prop));
                continue;
            }
            cJSON *po = cJSON_CreateObject();
            cJSON_AddStringToObject(po, "prop", p->prop);
            cJSON_AddStringToObject(po, "key", p->key);
            cJSON_AddStringToObject(po, "type", schema_prop_type_name(p->type));
            if (p->def) cJSON_AddItemToObject(po, "default", cJSON_Duplicate(p->def, 1));
            if (p->has_min) cJSON_AddNumberToObject(po, "min", p->min);
            if (p->has_max) cJSON_AddNumberToObject(po, "max", p->max);
            if (p->max_length >= 0) cJSON_AddNumberToObject(po, "max_length", p->max_length);
            cJSON_AddStringToObject(po, "since", p->since);
            if (p->desc[0]) cJSON_AddStringToObject(po, "desc", p->desc);
            cJSON_AddItemToArray(props, po);
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static cJSON *catalog_json(const Ctx *ctx) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", e->key);
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "source", e->source == MAP_SOURCE_OFFICIAL ? "official" : "community");
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "assets/%s", e->path); // the catalogue's file (maps/v7/, maps/custom/, v3 maps/)
        cJSON_AddStringToObject(o, "path", path);
        cJSON_AddBoolToObject(o, "rotation", e->rotation);
        cJSON_AddStringToObject(o, "creator", e->creator ? e->creator : "");
        cJSON_AddItemToArray(arr, o);
    }
    (void) ctx;
    return arr;
}

typedef struct {
    const char *uri, *name, *title, *description, *mime;
} StaticResource;

static const StaticResource k_resources[] = {
    {"krunker://map/current", "map-current", "Current map", "The focused project's map JSON (editor form for live projects)", "application/json"},
    {"krunker://map/current/summary", "map-summary", "Current map summary", "map_info of the focused project", "application/json"},
    {"krunker://prefabs", "prefabs", "Prefab catalog", "Every prefab (v3.1.1 + v7.1.4) with flags, default size and allowed properties", "application/json"},
    {"krunker://properties", "properties", "Object property schema", "Friendly name, map key, type, range and availability of every object property", "application/json"},
    {"krunker://textures", "textures", "Textures", "Texture list (t index / name, variants)", "application/json"},
    {"krunker://triggers", "triggers", "Trigger spec", "Trigger events, actions, conditions and emitter sounds (v7.1.4; native = simulated by KrunkNative)", "application/json"},
    {"krunker://catalog", "catalog", "Map catalog", "Official and community maps shipped in assets/maps", "application/json"},
    {"krunker://config", "config", "Map settings schema", "Top-level map keys and the game / server config lists", "application/json"},
    {"krunker://limits", "limits", "Limits", "Object / spawn / trigger / size limits (v7.1.4)", "application/json"},
    {"krunker://docs/editor-mcp", "docs-editor-mcp", "kn_editor_mcp reference", "tools/editor-mcp/README.md", "text/markdown"},
    {NULL, NULL, NULL, NULL, NULL},
};

static cJSON *resources_list(Ctx *ctx) {
    cJSON *arr = cJSON_CreateArray();
    for (const StaticResource *r = k_resources; r->uri; r++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "uri", r->uri);
        cJSON_AddStringToObject(o, "name", r->name);
        cJSON_AddStringToObject(o, "title", r->title);
        cJSON_AddStringToObject(o, "description", r->description);
        cJSON_AddStringToObject(o, "mimeType", r->mime);
        cJSON_AddItemToArray(arr, o);
    }
    for (int i = 0; i < ctx->project_count; i++) {
        const Project *p = ctx->projects[i];
        cJSON *o = cJSON_CreateObject();
        char uri[128], name[200];
        snprintf(uri, sizeof(uri), "krunker://map/%s", p->id);
        snprintf(name, sizeof(name), "Map %s (%s)", p->name, p->id);
        cJSON_AddStringToObject(o, "uri", uri);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddStringToObject(o, "mimeType", "application/json");
        cJSON_AddItemToArray(arr, o);
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/doc/v7", ctx->root);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const size_t n = strlen(e->d_name);
            if (n < 4 || strcmp(e->d_name + n - 3, ".md")) continue;
            cJSON *o = cJSON_CreateObject();
            char uri[PATH_MAX];
            snprintf(uri, sizeof(uri), "krunker://docs/v7/%.*s", (int) (n - 3), e->d_name);
            cJSON_AddStringToObject(o, "uri", uri);
            cJSON_AddStringToObject(o, "name", e->d_name);
            cJSON_AddStringToObject(o, "mimeType", "text/markdown");
            cJSON_AddItemToArray(arr, o);
        }
        closedir(d);
    }
    return arr;
}

static cJSON *resource_templates(void) {
    return cJSON_Parse(S([
        {"uriTemplate":"krunker://map/{project}","name":"map","title":"Project map","description":"Map JSON of an open project (id, basename or name)","mimeType":"application/json"},
        {"uriTemplate":"krunker://prefabs/{name}","name":"prefab","title":"Prefab schema","description":"One prefab with its full property schema","mimeType":"application/json"},
        {"uriTemplate":"krunker://docs/v7/{name}","name":"v7-doc","title":"v7 spec","description":"doc/v7/<name>.md (map-logic, editor, krunkscript)","mimeType":"text/markdown"}
    ]));
}

cJSON *mcp_map_info(Ctx *ctx, Project *p); // tools_project.c

static int resource_read(Ctx *ctx, const char *uri, cJSON *contents, char *err, const size_t errsz) {
    char *text = NULL;
    const char *mime = "application/json";
    cJSON *json = NULL;
    if (!strncmp(uri, "krunker://map/", 14)) {
        const char *ref = uri + 14;
        int summary = 0;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", ref);
        char *slash = strchr(buf, '/');
        if (slash && !strcmp(slash, "/summary")) {
            *slash = 0;
            summary = 1;
        }
        Project *p;
        if (project_resolve(ctx, strcmp(buf, "current") ? buf : NULL, &p, err, errsz) != 0) return -1;
        if (summary) json = mcp_map_info(ctx, p);
        else text = jsonw_print(p->doc->root, JSONW_COMPACT);
    } else if (!strcmp(uri, "krunker://prefabs")) json = prefab_catalog(-1);
    else if (!strncmp(uri, "krunker://prefabs/", 18)) {
        const int id = schema_prefab_find(uri + 18);
        if (id < 0) {
            snprintf(err, errsz, "unknown prefab %s", uri + 18);
            return -1;
        }
        cJSON *arr = prefab_catalog(id);
        json = cJSON_DetachItemFromArray(arr, 0);
        cJSON_Delete(arr);
    } else if (!strcmp(uri, "krunker://properties")) json = cJSON_Duplicate(js_get(schema_json(), "properties"), 1);
    else if (!strcmp(uri, "krunker://textures")) json = cJSON_Duplicate(js_get(schema_json(), "textures"), 1);
    else if (!strcmp(uri, "krunker://triggers")) json = cJSON_Duplicate(schema_triggers(), 1);
    else if (!strcmp(uri, "krunker://catalog")) json = catalog_json(ctx);
    else if (!strcmp(uri, "krunker://limits")) json = cJSON_Duplicate(schema_limits(), 1);
    else if (!strcmp(uri, "krunker://config")) {
        json = cJSON_CreateObject();
        cJSON_AddItemToObject(json, "map_keys", cJSON_Duplicate(js_get(schema_json(), "map_keys"), 1));
        cJSON_AddItemToObject(json, "game_config", cJSON_Duplicate(js_get(schema_json(), "game_config"), 1));
        cJSON_AddItemToObject(json, "server_config", cJSON_Duplicate(js_get(schema_json(), "server_config"), 1));
        cJSON_AddItemToObject(json, "modes", cJSON_Duplicate(js_get(schema_json(), "modes"), 1));
        cJSON_AddItemToObject(json, "classes", cJSON_Duplicate(js_get(schema_json(), "classes"), 1));
    } else if (!strcmp(uri, "krunker://docs/editor-mcp")) {
        text = read_root_file(ctx, "tools/editor-mcp/README.md");
        mime = "text/markdown";
    } else if (!strncmp(uri, "krunker://docs/v7/", 18)) {
        const char *name = uri + 18;
        if (strchr(name, '/') || strstr(name, "..")) {
            snprintf(err, errsz, "bad doc name");
            return -1;
        }
        char rel[PATH_MAX];
        snprintf(rel, sizeof(rel), "doc/v7/%s.md", name);
        text = read_root_file(ctx, rel);
        mime = "text/markdown";
    } else {
        snprintf(err, errsz, "unknown resource %s", uri);
        return -1;
    }
    if (json) {
        text = jsonw_print(json, JSONW_COMPACT);
        cJSON_Delete(json);
    }
    if (!text) {
        snprintf(err, errsz, "resource %s is not available", uri);
        return -1;
    }
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "uri", uri);
    cJSON_AddStringToObject(c, "mimeType", mime);
    cJSON_AddStringToObject(c, "text", text);
    cJSON_AddItemToArray(contents, c);
    free(text);
    return 0;
}

// ---------------------------------------------------------------- prompts

typedef struct {
    const char *name, *title, *description, *args_json, *text;
} PromptDef;

static const PromptDef k_prompts[] = {
    {"build_arena", "Build an arena map", "Block out a new team arena: room, cover, spawns, validation and a look at it",
     S([{"name":"name","description":"map name","required":false},{"name":"size","description":"floor size in units (default 400)","required":false}]),
     "Build a new Krunker team arena named \"{name}\" about {size} units wide with kn_editor_mcp:\n"
     "1. map_new {template:\"blank\", name}. 2. generate_box_room for the outer walls (leave 2 doors), then cover with "
     "objects_create / fill_area (crates, walls of height 8-20). 3. spawns_add 4 spawns per team (team 1 west, team 2 east), "
     "facing the centre. 4. map_validate and fix every error and warning (spawns inside solids, unreachable spawns). "
     "5. render_multiview to check the layout, playtest {bots:4} to check flow. 6. map_save_as into the workspace."},
    {"review_map", "Review a map", "Validate, look at and summarise a map with concrete fixes",
     S([{"name":"project","description":"project id / name (default: focused)","required":false}]),
     "Review the Krunker map {project}: run map_info, map_validate, render_multiview and render_topdown of the whole map, "
     "then list concrete problems (unreachable or stacked spawns, objects outside the playable area, missing collision, trigger "
     "references to missing interfaces, limits) with object ids and the exact objects_update / objects_delete calls that fix them."},
    {"fix_validation", "Fix validation errors", "Iterate map_validate -> edits -> map_validate until clean",
     S([{"name":"project","description":"project id / name (default: focused)","required":false}]),
     "Run map_validate on {project}. For each error (then warning), make the smallest edit that resolves it (objects_update, "
     "objects_transform snap/move, spawns_update, triggers_update), re-validate after each batch, and use undo if a change makes "
     "things worse. Finish with a render_multiview and a summary of the changes (history shows them)."},
    {NULL, NULL, NULL, NULL, NULL},
};

static cJSON *prompts_list(void) {
    cJSON *arr = cJSON_CreateArray();
    for (const PromptDef *p = k_prompts; p->name; p++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddStringToObject(o, "title", p->title);
        cJSON_AddStringToObject(o, "description", p->description);
        cJSON_AddItemToObject(o, "arguments", cJSON_Parse(p->args_json));
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static cJSON *prompt_get(const char *name, const cJSON *args, char *err, const size_t errsz) {
    for (const PromptDef *p = k_prompts; p->name; p++) {
        if (strcmp(p->name, name)) continue;
        Sb sb = {0};
        for (const char *s = p->text; *s; s++) {
            if (*s == '{') {
                const char *e = strchr(s, '}');
                if (e) {
                    char key[64];
                    snprintf(key, sizeof(key), "%.*s", (int) (e - s - 1), s + 1);
                    const char *v = js_str(args, key, NULL);
                    if (!v) v = !strcmp(key, "size") ? "400" : !strcmp(key, "project") ? "(the focused project)" : "New Arena";
                    sb_puts(&sb, v);
                    s = e;
                    continue;
                }
            }
            sb_putc(&sb, *s);
        }
        char *text = sb_take(&sb);
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "description", p->description);
        cJSON *msgs = cJSON_AddArrayToObject(res, "messages");
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON *c = cJSON_AddObjectToObject(m, "content");
        cJSON_AddStringToObject(c, "type", "text");
        cJSON_AddStringToObject(c, "text", text ? text : "");
        cJSON_AddItemToArray(msgs, m);
        free(text);
        return res;
    }
    snprintf(err, errsz, "unknown prompt %s", name);
    return NULL;
}

// ---------------------------------------------------------------- JSON-RPC

static cJSON *rpc_result(const cJSON *id, cJSON *result) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(r, "result", result ? result : cJSON_CreateObject());
    return r;
}

static cJSON *rpc_error(const cJSON *id, const int code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return r;
}

static const char *k_instructions =
    "Krunker map editor (KrunkNative kn_editor_mcp). Work on projects: local map files (map_open / map_new) and, when a "
    "KrunkNative editor runs with its bus (client_bin --editor --mcp-bus), live editor projects (projects_list). Every tool "
    "takes an optional `project` (id, path, basename or map name; default: the focused one). Objects have stable ids "
    "(objects_query / object_get); all edits are batched, validated against the prefab schema (krunker://prefabs) and undoable "
    "(undo / redo / history). Coordinates: Krunker units, y up, p is the bottom centre of an object, rotations in radians. "
    "Check work with map_validate, render_multiview / render_view / render_topdown (PNG images) and playtest. Save with "
    "map_save / map_save_as (only under the allowed directories); map_compile produces the publishable form.";

static cJSON *handle_initialize(const cJSON *params) {
    const char *want = js_str(params, "protocolVersion", MCP_PROTOCOL_LATEST);
    snprintf(g_protocol, sizeof(g_protocol), "%s", MCP_PROTOCOL_LATEST);
    for (int i = 0; k_protocols[i]; i++) if (!strcmp(k_protocols[i], want)) snprintf(g_protocol, sizeof(g_protocol), "%s", want);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "protocolVersion", g_protocol);
    cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
    cJSON_AddFalseToObject(cJSON_AddObjectToObject(caps, "tools"), "listChanged");
    cJSON *res = cJSON_AddObjectToObject(caps, "resources");
    cJSON_AddFalseToObject(res, "subscribe");
    cJSON_AddFalseToObject(res, "listChanged");
    cJSON_AddFalseToObject(cJSON_AddObjectToObject(caps, "prompts"), "listChanged");
    cJSON_AddObjectToObject(caps, "logging");
    cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
    cJSON_AddStringToObject(info, "name", "kn_editor_mcp");
    cJSON_AddStringToObject(info, "title", "Krunker Map Editor (KrunkNative)");
    cJSON_AddStringToObject(info, "version", KN_MCP_VERSION);
    cJSON_AddStringToObject(r, "instructions", k_instructions);
    const cJSON *client = js_get(params, "clientInfo");
    log_msg(LOG_INFO, "mcp", "initialize from %s %s (protocol %s)", js_str(client, "name", "?"), js_str(client, "version", ""), g_protocol);
    return r;
}

static cJSON *handle_one(Ctx *ctx, const cJSON *msg) {
    if (!cJSON_IsObject(msg)) return rpc_error(NULL, -32600, "Invalid Request");
    const cJSON *id = js_get(msg, "id");
    const char *method = js_str(msg, "method", NULL);
    const cJSON *params = js_get(msg, "params");
    const int notification = !id;
    if (!method) {
        // a response to a server request (we send none) or garbage
        return notification ? NULL : rpc_error(id, -32600, "Invalid Request");
    }
    if (!strncmp(method, "notifications/", 14)) {
        if (!strcmp(method, "notifications/initialized")) g_initialized = 1;
        return NULL;
    }
    if (notification) return NULL;
    if (!strcmp(method, "initialize")) return rpc_result(id, handle_initialize(params));
    if (!strcmp(method, "ping")) return rpc_result(id, NULL);
    if (!strcmp(method, "tools/list")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "tools", tools_list());
        return rpc_result(id, r);
    }
    if (!strcmp(method, "tools/call")) {
        const char *name = js_str(params, "name", NULL);
        if (!name) return rpc_error(id, -32602, "tools/call needs params.name");
        cJSON *res = mcp_call_tool(ctx, name, js_get(params, "arguments"));
        if (!res) {
            char m[300];
            snprintf(m, sizeof(m), "Unknown tool: %s", name);
            return rpc_error(id, -32602, m);
        }
        return rpc_result(id, res);
    }
    if (!strcmp(method, "resources/list")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "resources", resources_list(ctx));
        return rpc_result(id, r);
    }
    if (!strcmp(method, "resources/templates/list")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "resourceTemplates", resource_templates());
        return rpc_result(id, r);
    }
    if (!strcmp(method, "resources/read")) {
        const char *uri = js_str(params, "uri", NULL);
        if (!uri) return rpc_error(id, -32602, "resources/read needs params.uri");
        cJSON *r = cJSON_CreateObject();
        cJSON *contents = cJSON_AddArrayToObject(r, "contents");
        char err[512];
        if (resource_read(ctx, uri, contents, err, sizeof(err)) != 0) {
            cJSON_Delete(r);
            return rpc_error(id, -32002, err);
        }
        return rpc_result(id, r);
    }
    if (!strcmp(method, "resources/subscribe") || !strcmp(method, "resources/unsubscribe")) return rpc_result(id, NULL);
    if (!strcmp(method, "prompts/list")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "prompts", prompts_list());
        return rpc_result(id, r);
    }
    if (!strcmp(method, "prompts/get")) {
        char err[256];
        cJSON *r = prompt_get(js_str(params, "name", ""), js_get(params, "arguments"), err, sizeof(err));
        return r ? rpc_result(id, r) : rpc_error(id, -32602, err);
    }
    if (!strcmp(method, "logging/setLevel")) {
        const int level = log_level_parse(js_str(params, "level", ""));
        if (level < 0) return rpc_error(id, -32602, "unknown log level");
        ctx->log_level = level;
        return rpc_result(id, NULL);
    }
    if (!strcmp(method, "completion/complete")) {
        cJSON *r = cJSON_CreateObject();
        cJSON *c = cJSON_AddObjectToObject(r, "completion");
        cJSON *values = cJSON_AddArrayToObject(c, "values");
        const cJSON *arg = js_get(params, "argument");
        const char *prefix = js_str(arg, "value", "");
        if (!strcmp(js_str(arg, "name", ""), "name")) {
            for (int i = 0; i < schema_prefab_count() && cJSON_GetArraySize(values) < 100; i++) {
                const char *n = schema_prefab(i)->name;
                if (!strncasecmp(n, prefix, strlen(prefix))) cJSON_AddItemToArray(values, cJSON_CreateString(n));
            }
        } else {
            for (int i = 0; i < ctx->project_count; i++) cJSON_AddItemToArray(values, cJSON_CreateString(ctx->projects[i]->id));
        }
        return rpc_result(id, r);
    }
    char m[300];
    snprintf(m, sizeof(m), "Method not found: %s", method);
    return rpc_error(id, -32601, m);
}

cJSON *mcp_handle(Ctx *ctx, const cJSON *msg) {
    g_ctx = ctx;
    if (cJSON_IsArray(msg)) {
        if (!cJSON_GetArraySize(msg)) return rpc_error(NULL, -32600, "Invalid Request");
        cJSON *out = cJSON_CreateArray();
        const cJSON *it;
        cJSON_ArrayForEach(it, msg) {
            cJSON *r = handle_one(ctx, it);
            if (r) cJSON_AddItemToArray(out, r);
        }
        if (!cJSON_GetArraySize(out)) {
            cJSON_Delete(out);
            return NULL;
        }
        return out;
    }
    return handle_one(ctx, msg);
}

static void write_message(FILE *out, const cJSON *msg) {
    char *text = jsonw_print(msg, JSONW_COMPACT);
    if (!text) return;
    fputs(text, out);
    fputc('\n', out);
    fflush(out);
    free(text);
}

static void log_sink(const LogLevel level, const char *source, const char *msg) {
    if (!g_out || !g_initialized || !g_ctx || (int) level < g_ctx->log_level) return;
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "jsonrpc", "2.0");
    cJSON_AddStringToObject(n, "method", "notifications/message");
    cJSON *p = cJSON_AddObjectToObject(n, "params");
    cJSON_AddStringToObject(p, "level", log_level_name(level));
    cJSON_AddStringToObject(p, "logger", source);
    cJSON_AddStringToObject(p, "data", msg);
    write_message(g_out, n);
    cJSON_Delete(n);
}

int mcp_run_stdio(Ctx *ctx) {
    g_ctx = ctx;
    g_out = stdout;
    log_set_sink(log_sink);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        cJSON *msg = cJSON_ParseWithLength(line, (size_t) n);
        cJSON *res = msg ? mcp_handle(ctx, msg) : rpc_error(NULL, -32700, "Parse error");
        if (res) write_message(stdout, res);
        cJSON_Delete(res);
        cJSON_Delete(msg);
    }
    free(line);
    log_set_sink(NULL);
    return 0;
}
