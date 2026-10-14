// The AI panel's editor tools (editor_ai.h): the tools of tools/editor-mcp (kn_editor_mcp: map_info,
// objects_create, generate_box_room, render_view, script_set, map_validate, playtest, ...) run
// in-process against this editor's open maps. kn_editor_mcp's live mode normally reaches an
// editor over the editor bus socket; here its Live.loopback hands every bus request straight to
// ed_bus_local_request() (editor_bus_link.c), so edits land on the editor's own undo stack
// exactly as they would from Claude Code + kn_editor_mcp, with no socket and no permission
// dialog (the panel asks for approval before destructive tools itself).
//
// Builds without the tool core (KN_AI_EDITOR_TOOLS unset: the Windows client, where
// kn_editor_mcp is POSIX-only) offer no editor tools; the chat still works.
#include "editor_ai.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AI_TOOL_TEXT_MAX (24u * 1024u) // result text handed to the model

#ifdef KN_AI_EDITOR_TOOLS
#include "../../../tools/editor-mcp/src/mcp.h"
#include "../../../tools/editor-mcp/src/schema.h"
#include "../../../tools/editor-mcp/src/util.h"
#include <limits.h>
#include <unistd.h>

static Ctx g_ctx;
static int g_state; // 0 not tried, 1 ready, -1 failed
static char g_reason[256];

static cJSON *loopback(const char *op, const cJSON *args, char *err, const size_t errsz) { return ed_bus_local_request(op, args, err, errsz); }

static int tools_init(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.focus = -1;
    g_ctx.render_timeout_s = 90.0;
    g_ctx.log_level = LOG_WARN;
    live_init(&g_ctx.live);
    g_ctx.live.mode = 2; // the editor itself (loopback), never another one over the bus
    g_ctx.live.loopback = loopback;
    // root: the directory that holds assets/ (maps, textures); the workspace: scratch maps
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s..", assets_path());
    if (!realpath(probe, g_ctx.root) && !getcwd(g_ctx.root, sizeof(g_ctx.root))) snprintf(g_ctx.root, sizeof(g_ctx.root), ".");
    char ws[PATH_MAX];
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) snprintf(ws, sizeof(ws), "%s/krunknative/editor-ai", xdg);
    else if (getenv("HOME")) snprintf(ws, sizeof(ws), "%s/.local/share/krunknative/editor-ai", getenv("HOME"));
    else snprintf(ws, sizeof(ws), "/tmp/krunknative-editor-ai");
    if (mkdir_p(ws) != 0 || !realpath(ws, g_ctx.workspace)) snprintf(g_ctx.workspace, sizeof(g_ctx.workspace), "%s", ws);
    snprintf(probe, sizeof(probe), "%s/assets/maps", g_ctx.root);
    ctx_add_root(&g_ctx, probe);
    ctx_add_root(&g_ctx, g_ctx.workspace);
    log_set_stderr(0); // the `logs` tool keeps them; the client's stderr stays readable
    char err[256];
    if (schema_load(NULL, g_ctx.root, err, sizeof(err)) != 0) {
        snprintf(g_reason, sizeof(g_reason), "editor tools unavailable: %s", err);
        fprintf(stderr, "[editor] ai: %s\n", g_reason);
        return 0;
    }
    g_state = 1;
    return 1;
}

int ai_tools_available(void) { return tools_init(); }
const char *ai_tools_unavailable_reason(void) { return g_reason[0] ? g_reason : "editor tools unavailable"; }

// tools whose effect the user should confirm first: kn_editor_mcp's destructive ones (deletes,
// closing a project) and the ones that write files or replace a whole script
static const char *const k_confirm[] = {"map_save", "map_save_as", "map_compile", "prefab_save", "script_set", "project_close", NULL};

int ai_tool_known(const char *name) { return tools_init() && mcp_tool_def(name) != NULL; }

int ai_tool_needs_approval(const char *name) {
    const ToolDef *d = tools_init() ? mcp_tool_def(name) : NULL;
    if (!d) return 0;
    if (d->destructive) return 1;
    for (int i = 0; k_confirm[i]; i++) {
        if (!strcmp(k_confirm[i], name)) return 1;
    }
    return 0;
}

int ai_tool_read_only(const char *name) {
    const ToolDef *d = tools_init() ? mcp_tool_def(name) : NULL;
    return d && d->read_only;
}

cJSON *ai_tools_defs(void) {
    if (!tools_init()) return NULL;
    cJSON *list = mcp_tools_json();
    cJSON *out = cJSON_CreateArray();
    const cJSON *t;
    cJSON_ArrayForEach(t, list) {
        const char *name = js_str(t, "name", "");
        // the bus connection is the editor itself here: nothing to connect to
        if (!name[0] || !strcmp(name, "editor_connect")) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(o, "function");
        cJSON_AddStringToObject(fn, "name", name);
        cJSON_AddStringToObject(fn, "description", js_str(t, "description", ""));
        const cJSON *schema = js_get(t, "inputSchema");
        cJSON_AddItemToObject(fn, "parameters", schema ? cJSON_Duplicate(schema, 1) : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
        cJSON_AddItemToArray(out, o);
    }
    cJSON_Delete(list);
    return out;
}

// the tools' default project = the map shown in the editor
static void focus_shown(void) {
    char err[256];
    if (projects_refresh_live(&g_ctx, err, sizeof(err)) != 0 || !g_ed.shown) return;
    char id[32];
    snprintf(id, sizeof(id), ".%d", g_ed.shown->id);
    for (int i = 0; i < g_ctx.project_count; i++) {
        const char *pid = g_ctx.projects[i]->id;
        const size_t n = strlen(pid), m = strlen(id);
        if (g_ctx.projects[i]->kind == PROJ_LIVE && n > m && !strcmp(pid + n - m, id)) {
            g_ctx.focus = i;
            return;
        }
    }
}

cJSON *ai_tool_run(const char *name, const char *args_json) {
    cJSON *out = cJSON_CreateObject();
    if (!tools_init()) {
        cJSON_AddStringToObject(out, "text", ai_tools_unavailable_reason());
        cJSON_AddBoolToObject(out, "is_error", 1);
        return out;
    }
    cJSON *args = args_json && args_json[0] ? cJSON_Parse(args_json) : cJSON_CreateObject();
    if (!cJSON_IsObject(args)) {
        cJSON_Delete(args);
        cJSON_AddStringToObject(out, "text", "the tool arguments are not a JSON object");
        cJSON_AddBoolToObject(out, "is_error", 1);
        return out;
    }
    focus_shown();
    cJSON *res = mcp_call_tool(&g_ctx, name, args);
    cJSON_Delete(args);
    if (!res) {
        char m[200];
        snprintf(m, sizeof(m), "unknown tool %s", name);
        cJSON_AddStringToObject(out, "text", m);
        cJSON_AddBoolToObject(out, "is_error", 1);
        return out;
    }
    Sb text = {0};
    cJSON *images = cJSON_CreateArray();
    const cJSON *c;
    cJSON_ArrayForEach(c, js_get(res, "content")) {
        const char *type = js_str(c, "type", "");
        if (!strcmp(type, "text")) {
            if (text.len) sb_puts(&text, "\n");
            sb_puts(&text, js_str(c, "text", ""));
        } else if (!strcmp(type, "image")) {
            const char *data = js_str(c, "data", "");
            const char *mime = js_str(c, "mimeType", "image/png");
            Sb url = {0};
            sb_printf(&url, "data:%s;base64,", mime);
            sb_puts(&url, data);
            char *u = sb_take(&url);
            if (u) cJSON_AddItemToArray(images, cJSON_CreateString(u));
            free(u);
            if (text.len) sb_puts(&text, "\n");
            sb_printf(&text, "[image %d: %s, shown to the user in the editor]", cJSON_GetArraySize(images), mime);
        }
    }
    char *t = sb_take(&text);
    if (t && strlen(t) > AI_TOOL_TEXT_MAX) {
        t[AI_TOOL_TEXT_MAX] = 0;
        Sb more = {0};
        sb_puts(&more, t);
        sb_printf(&more, "\n[... cut at %u bytes]", (unsigned) AI_TOOL_TEXT_MAX);
        free(t);
        t = sb_take(&more);
    }
    cJSON_AddStringToObject(out, "text", t && t[0] ? t : "(no output)");
    free(t);
    cJSON_AddBoolToObject(out, "is_error", js_bool(res, "isError", 0));
    if (cJSON_GetArraySize(images)) cJSON_AddItemToObject(out, "images", images);
    else cJSON_Delete(images);
    cJSON_Delete(res);
    return out;
}
#else
int ai_tools_available(void) { return 0; }
const char *ai_tools_unavailable_reason(void) { return "this build has no editor tools (the AI can answer, not edit)"; }
int ai_tool_known(const char *name) { (void) name; return 0; }
int ai_tool_needs_approval(const char *name) { (void) name; return 0; }
int ai_tool_read_only(const char *name) { (void) name; return 0; }
cJSON *ai_tools_defs(void) { return NULL; }
cJSON *ai_tool_run(const char *name, const char *args_json) {
    (void) args_json;
    cJSON *out = cJSON_CreateObject();
    char m[200];
    snprintf(m, sizeof(m), "%s: %s", name ? name : "tool", ai_tools_unavailable_reason());
    cJSON_AddStringToObject(out, "text", m);
    cJSON_AddBoolToObject(out, "is_error", 1);
    return out;
}
#endif
