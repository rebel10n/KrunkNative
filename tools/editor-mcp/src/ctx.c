// kn_editor_mcp context helpers shared by the server (main.c) and the editor's AI panel, which
// runs these tools in-process (client/src/editor/editor_ai_tools.c): allowed map directories and
// path safety.
#define _GNU_SOURCE
#include "ctx.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int ctx_add_root(Ctx *ctx, const char *dir) {
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
