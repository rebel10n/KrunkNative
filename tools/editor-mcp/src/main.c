// kn_editor_mcp: a Model Context Protocol server for Krunker maps (KrunkNative).
// Reference: tools/editor-mcp/README.md.
#define _GNU_SOURCE
#include "ctx.h"
#include "jsonw.h"
#include "mcp.h"
#include "schema.h"
#include "util.h"
#include <map_catalog.h>
#include <shared.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f,
        "kn_editor_mcp %s - Krunker map editor MCP server (Krunker Box Office)\n"
        "usage: kn_editor_mcp [options]          MCP over stdio (default)\n"
        "  --root DIR             KrunkNative checkout (assets, client_bin); default: auto-detect\n"
        "  --workspace DIR        scratch dir for new maps / prefab library (allowed for writes)\n"
        "                         default: $XDG_DATA_HOME/krunknative/editor-mcp\n"
        "  --allow DIR            another directory maps may be read from / written to (repeatable)\n"
        "  --open MAP             open a map at start (path or catalog name; repeatable)\n"
        "  --schema FILE          schema JSON (default: <root>/tools/editor-mcp/data/schema.json, else embedded)\n"
        "  --client PATH          client_bin used for renders (default: <root>/cmake-build-{release,debug}/client_bin)\n"
        "  --read-only            refuse every edit / save\n"
        "  --live                 require the live editor bus (fail tools when no editor runs)\n"
        "  --no-live              never attach to an editor (file mode only)\n"
        "                         default: auto (editor projects appear when an editor hosts the bus)\n"
        "  --bus-dir DIR          editor bus directory (default: KRUNKNATIVE_EDITOR_BUS_DIR, XDG_RUNTIME_DIR/krunknative)\n"
        "  --scopes LIST          permission scopes to request: read,edit,project (default all)\n"
        "  --permission-timeout S seconds to wait for the editor's permission dialog (default 300)\n"
        "  --http HOST:PORT       serve MCP Streamable HTTP instead of stdio (POST /mcp)\n"
        "  --announce FILE        with --http: write the URL to FILE\n"
        "  --call TOOL [JSON]     run one tool, print the result JSON and exit\n"
        "  --quiet                no log lines on stderr\n"
        "  --version | --help\n",
        KN_MCP_VERSION);
}

static int add_root(Ctx *ctx, const char *dir) {
    if (ctx->root_count >= KN_MCP_MAX_ROOTS) return -1;
    char resolved[PATH_MAX];
    if (!realpath(dir, resolved)) return -1;
    for (int i = 0; i < ctx->root_count; i++) if (!strcmp(ctx->roots[i], resolved)) return 0;
    snprintf(ctx->roots[ctx->root_count++], PATH_MAX, "%s", resolved);
    return 0;
}

cJSON *ctx_roots_json(const Ctx *ctx) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < ctx->root_count; i++) cJSON_AddItemToArray(a, cJSON_CreateString(ctx->roots[i]));
    return a;
}

int ctx_safe_path(Ctx *ctx, const char *path, const int for_write, char *out, const size_t size, char *err, const size_t errsz) {
    if (!path || !path[0]) {
        snprintf(err, errsz, "empty path");
        return -1;
    }
    char joined[PATH_MAX];
    if (path[0] == '/') snprintf(joined, sizeof(joined), "%s", path);
    else if (path[0] == '~' && path[1] == '/' && getenv("HOME")) snprintf(joined, sizeof(joined), "%s%s", getenv("HOME"), path + 1);
    else snprintf(joined, sizeof(joined), "%s/%s", ctx->root, path);
    if (path_resolve(joined, out, size) != 0) {
        snprintf(err, errsz, "cannot resolve %s (missing directory?)", path);
        return -1;
    }
    int ok = 0;
    for (int i = 0; i < ctx->root_count && !ok; i++) ok = path_inside(out, ctx->roots[i]);
    if (!ok) {
        snprintf(err, errsz, "%s is outside the allowed map directories (see projects_list.allowed_dirs; add --allow DIR)", out);
        return -1;
    }
    if (for_write) {
        const size_t n = strlen(out);
        if (n < 6 || strcasecmp(out + n - 5, ".json")) {
            snprintf(err, errsz, "maps are saved as .json files");
            return -1;
        }
    }
    return 0;
}

static int detect_root(char *out, const size_t size, const char *argv0) {
    const char *env = getenv("KRUNKNATIVE_ROOT");
    char probe[PATH_MAX];
    if (env && env[0]) {
        snprintf(probe, sizeof(probe), "%s/assets/maps", env);
        if (dir_exists(probe) && realpath(env, out)) return 0;
    }
    // the binary in <root>/cmake-build-*/ or <root>/tools/editor-mcp/...
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0 && argv0) n = (ssize_t) strlen(strncpy(self, argv0, sizeof(self) - 1));
    if (n > 0) {
        self[n] = 0;
        for (int up = 0; up < 4; up++) {
            char *slash = strrchr(self, '/');
            if (!slash) break;
            *slash = 0;
            snprintf(probe, sizeof(probe), "%s/assets/maps", self);
            if (dir_exists(probe) && realpath(self, out)) return 0;
        }
    }
    if (getcwd(self, sizeof(self))) {
        snprintf(probe, sizeof(probe), "%s/assets/maps", self);
        if (dir_exists(probe)) {
            snprintf(out, size, "%s", self);
            return 0;
        }
    }
    snprintf(probe, sizeof(probe), "%s/src/KrunkNative/assets/maps", getenv("HOME") ? getenv("HOME") : "");
    if (dir_exists(probe)) {
        snprintf(probe, sizeof(probe), "%s/src/KrunkNative", getenv("HOME"));
        if (realpath(probe, out)) return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    static Ctx ctx;
    ctx.focus = -1;
    ctx.render_timeout_s = 90.0;
    ctx.log_level = LOG_WARN;
    live_init(&ctx.live);
    const char *root = NULL, *workspace = NULL, *http = NULL, *announce = NULL, *call_tool = NULL, *call_args = NULL;
    const char *allow[KN_MCP_MAX_ROOTS];
    const char *opens[64];
    int allow_count = 0, open_count = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
#define NEED_ARG() do { if (!next) { fprintf(stderr, "%s needs an argument\n", a); return 2; } i++; } while (0)
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            printf("kn_editor_mcp %s\n", KN_MCP_VERSION);
            return 0;
        } else if (!strcmp(a, "--root")) { NEED_ARG(); root = next; }
        else if (!strcmp(a, "--workspace")) { NEED_ARG(); workspace = next; }
        else if (!strcmp(a, "--allow")) { NEED_ARG(); if (allow_count < KN_MCP_MAX_ROOTS) allow[allow_count++] = next; }
        else if (!strcmp(a, "--open")) { NEED_ARG(); if (open_count < 64) opens[open_count++] = next; }
        else if (!strcmp(a, "--schema")) { NEED_ARG(); snprintf(ctx.schema_path, sizeof(ctx.schema_path), "%s", next); }
        else if (!strcmp(a, "--client")) { NEED_ARG(); if (!realpath(next, ctx.client_bin)) { fprintf(stderr, "--client: %s not found\n", next); return 2; } }
        else if (!strcmp(a, "--read-only")) ctx.read_only = 1;
        else if (!strcmp(a, "--live")) ctx.live.mode = 2;
        else if (!strcmp(a, "--no-live")) ctx.live.mode = 0;
        else if (!strcmp(a, "--bus-dir")) { NEED_ARG(); snprintf(ctx.live.bus_dir, sizeof(ctx.live.bus_dir), "%s", next); }
        else if (!strcmp(a, "--scopes")) { NEED_ARG(); ctx.live.want_scopes = edbus_scope_parse(next); if (!ctx.live.want_scopes) ctx.live.want_scopes = EDBUS_SCOPE_READ; }
        else if (!strcmp(a, "--permission-timeout")) { NEED_ARG(); ctx.live.permission_timeout_s = atof(next) > 0 ? atof(next) : 300; }
        else if (!strcmp(a, "--http")) { NEED_ARG(); http = next; }
        else if (!strcmp(a, "--announce")) { NEED_ARG(); announce = next; }
        else if (!strcmp(a, "--call")) {
            NEED_ARG();
            call_tool = next;
            if (i + 1 < argc && argv[i + 1][0] != '-') call_args = argv[++i];
        } else if (!strcmp(a, "--quiet")) log_set_stderr(0);
        else {
            fprintf(stderr, "unknown option %s (see --help)\n", a);
            return 2;
        }
#undef NEED_ARG
    }

    if (root) {
        if (!realpath(root, ctx.root)) {
            fprintf(stderr, "--root %s: %s\n", root, strerror(errno));
            return 2;
        }
    } else if (detect_root(ctx.root, sizeof(ctx.root), argv[0]) != 0) {
        fprintf(stderr, "cannot find the KrunkNative checkout: pass --root DIR\n");
        return 2;
    }
    // the shared map catalogue reads assets relative to the working directory
    if (chdir(ctx.root) != 0) {
        fprintf(stderr, "cannot enter %s\n", ctx.root);
        return 2;
    }

    char dir[PATH_MAX];
    if (workspace) snprintf(dir, sizeof(dir), "%s", workspace);
    else {
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) snprintf(dir, sizeof(dir), "%s/krunknative/editor-mcp", xdg);
        else snprintf(dir, sizeof(dir), "%s/.local/share/krunknative/editor-mcp", getenv("HOME") ? getenv("HOME") : "/tmp");
    }
    if (mkdir_p(dir) != 0 || !realpath(dir, ctx.workspace)) {
        fprintf(stderr, "cannot create the workspace %s\n", dir);
        return 2;
    }
    snprintf(dir, sizeof(dir), "%s/assets/maps", ctx.root);
    add_root(&ctx, dir);
    add_root(&ctx, ctx.workspace);
    for (int i = 0; i < allow_count; i++) {
        if (add_root(&ctx, allow[i]) != 0) fprintf(stderr, "--allow %s: not a directory\n", allow[i]);
    }

    char err[512];
    if (schema_load(ctx.schema_path, ctx.root, err, sizeof(err)) != 0) {
        fprintf(stderr, "schema: %s\n", err);
        return 2;
    }
    log_msg(LOG_DEBUG, "mcp", "root %s, workspace %s, schema %s (%d prefabs)", ctx.root, ctx.workspace, schema_source(), schema_prefab_count());

    for (int i = 0; i < open_count; i++) {
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "path", opens[i]);
        cJSON *res = mcp_call_tool(&ctx, "map_open", args);
        if (res && js_bool(res, "isError", 0)) {
            const cJSON *c = cJSON_GetArrayItem(js_get(res, "content"), 0);
            fprintf(stderr, "--open %s: %s\n", opens[i], js_str(c, "text", "failed"));
        }
        cJSON_Delete(res);
        cJSON_Delete(args);
    }

    if (call_tool) {
        cJSON *args = call_args ? cJSON_Parse(call_args) : cJSON_CreateObject();
        if (!args) {
            fprintf(stderr, "--call: arguments are not valid JSON\n");
            return 2;
        }
        cJSON *res = mcp_call_tool(&ctx, call_tool, args);
        cJSON_Delete(args);
        if (!res) {
            fprintf(stderr, "unknown tool %s\n", call_tool);
            return 2;
        }
        char *text = jsonw_print(res, JSONW_PRETTY);
        printf("%s\n", text ? text : "{}");
        free(text);
        const int failed = js_bool(res, "isError", 0);
        cJSON_Delete(res);
        return failed ? 1 : 0;
    }
    if (http) return mcp_run_http(&ctx, http, announce);
    return mcp_run_stdio(&ctx);
}
