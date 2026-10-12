// script_get / script_set: a map's KrunkScript (v7.1.4 "scripts": {client, server}, UTF-8 base64
// in exports, omitted when equal to the editor's template; doc/v7/editor.md 1.8 and WP5).
// Live projects go through the edit transaction like every other edit: the "scripts" key
// becomes a bus map_set, which the editor keeps as the project's scripts (its KrunkScript
// window, client/src/editor/editor_script.c, follows).
#include "mcp.h"
#include "util.h"
#include "validate.h"
#include "editor_model.h"
#include <stdlib.h>
#include <string.h>

static const char *const k_sides[2] = {"client", "server"};

static int side_arg(const cJSON *args, int *both) {
    const char *s = js_str(args, "side", "both");
    *both = !strcmp(s, "both");
    if (*both || !strcmp(s, "client")) return 0;
    if (!strcmp(s, "server")) return 1;
    return -1;
}

// the source of side k: the decoded scripts.<k>, else the template (what the editor shows)
static char *script_source(const cJSON *root, const int k, int *is_template, int *present) {
    const cJSON *v = js_get(js_get(root, "scripts"), k_sides[k]);
    *present = cJSON_IsString(v) && v->valuestring[0];
    char *src = NULL;
    if (*present) src = ks_bridge_decode(v->valuestring, NULL);
    const char *tpl = ed_model_script_template(k_sides[k]);
    if (!src) src = strdup(tpl);
    *is_template = src && !strcmp(src, tpl);
    return src;
}

static cJSON *side_json(const cJSON *root, const int k, const int with_source) {
    int is_template, present;
    char *src = script_source(root, k, &is_template, &present);
    cJSON *o = cJSON_CreateObject();
    if (!src) return o;
    const size_t len = strlen(src);
    int lines = 1;
    for (size_t i = 0; i < len; i++) lines += src[i] == '\n';
    if (with_source) cJSON_AddStringToObject(o, "source", src);
    cJSON_AddBoolToObject(o, "in_map", present);
    cJSON_AddBoolToObject(o, "is_template", is_template);
    cJSON_AddNumberToObject(o, "bytes", (double) len);
    cJSON_AddNumberToObject(o, "lines", lines);
    cJSON *diags = cJSON_AddArrayToObject(o, "diagnostics");
    int available = 0;
    const int errors = ks_bridge_check_source(src, len, k, diags, &available);
    cJSON_AddBoolToObject(o, "ok", !errors);
    if (!available) cJSON_AddStringToObject(o, "checker", ks_bridge_status());
    free(src);
    return o;
}

static int t_script_get(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int both;
    const int side = side_arg(args, &both);
    if (side < 0) return tr_error(r, "side: \"client\", \"server\" or \"both\"");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "project", p->id);
    cJSON *sc = cJSON_AddObjectToObject(o, "scripts");
    for (int k = 0; k < 2; k++) {
        if (both || k == side) cJSON_AddItemToObject(sc, k_sides[k], side_json(p->doc->root, k, js_bool(args, "source", 1)));
    }
    tr_json(r, o);
    return 0;
}

static int t_script_set(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    int both;
    const int side = side_arg(args, &both);
    if (side < 0 || both) return tr_error(r, "side: \"client\" or \"server\"");
    const cJSON *src = js_get(args, "source");
    if (!cJSON_IsString(src)) return tr_error(r, "source: the script text (\"\" = back to the template)");
    // a check first: errors are refused unless force (the map would not run)
    const size_t len = strlen(src->valuestring);
    cJSON *diags = cJSON_CreateArray();
    int available = 0;
    const int errors = len ? ks_bridge_check_source(src->valuestring, len, side, diags, &available) : 0;
    if (errors && !js_bool(args, "force", 0)) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "error", "the script does not compile: fix it (or pass force:true)");
        cJSON_AddItemToObject(o, "diagnostics", diags);
        tr_json(r, o);
        r->is_error = 1;
        return 0;
    }
    cJSON_Delete(diags);
    Edit e;
    char err[512];
    if (edit_begin(ctx, p, side ? "server script" : "client script", &e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    cJSON *root = p->doc->root;
    cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "scripts");
    if (!cJSON_IsObject(sc)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "scripts");
        sc = cJSON_AddObjectToObject(root, "scripts");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(sc, k_sides[side]);
    // the editor's export form: UTF-8 base64, nothing for "" or the template (E:3519-3531)
    if (len && strcmp(src->valuestring, ed_model_script_template(k_sides[side]))) {
        char *b64 = ed_model_b64_encode(src->valuestring);
        if (!b64) {
            edit_abort(&e);
            return tr_error(r, "out of memory");
        }
        cJSON_AddStringToObject(sc, k_sides[side], b64);
        free(b64);
    }
    if (!cJSON_GetArraySize(sc)) cJSON_DeleteItemFromObjectCaseSensitive(root, "scripts");
    if (edit_commit(&e, err, sizeof(err)) != 0) return tr_error(r, "%s", err);
    edit_end(&e);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "project", p->id);
    cJSON_AddStringToObject(o, "side", k_sides[side]);
    cJSON_AddItemToObject(o, "script", side_json(p->doc->root, side, 0));
    tr_json(r, o);
    return 0;
}

const ToolDef g_tools_script[] = {
    {"script_get", "Read the map's KrunkScript",
     "The map's KrunkScript sources (scripts.client / scripts.server, decoded from the export's base64; a side the "
     "map does not carry shows the editor's template) with line counts and the checker's diagnostics "
     "(ksc / the editor's script window: the first error, then warnings).",
     S({"type":"object","properties":{PROJECT_ARG,"side":{"type":"string","enum":["client","server","both"],"description":"default both"},
        "source":{"type":"boolean","description":"include the source text (default true)"}}}),
     1, 0, 1, 0, t_script_get},
    {"script_set", "Write the map's KrunkScript",
     "Replaces one side's KrunkScript source. It is stored as the editor exports it (UTF-8 base64; \"\" or the "
     "template removes the key). A source that does not compile is refused unless force:true. One undo step; "
     "a live editor project's script window follows.",
     S({"type":"object","properties":{PROJECT_ARG,"side":{"type":"string","enum":["client","server"]},
        "source":{"type":"string"},"force":{"type":"boolean","description":"store even with compile errors"}},
        "required":["side","source"]}),
     0, 0, 1, 0, t_script_set},
    {0},
};
