// Live mode: kn_editor_mcp as a client of the editor bus (shared/include/editor_bus.h,
// protocol v1). Connection, hello and the permission handshake happen lazily at the first tool
// call that needs an editor (Claude Code starts MCP servers at session start; a permission
// dialog then would block `initialize`). A denial is cached: tools fail with a clear error and
// only `editor_connect {retry:true}` asks again.
#define _GNU_SOURCE
#include "ctx.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void live_init(Live *l) {
    memset(l, 0, sizeof(*l));
    l->mode = 1;
    l->want_scopes = EDBUS_SCOPE_ALL;
    l->next_id = 1;
    l->permission_timeout_s = 300.0;
    edbus_conn_init(&l->conn, -1);
}

static int bus_paths(Live *l, EdBusPaths *p) {
    return edbus_paths_init(p, l->bus_dir[0] ? l->bus_dir : NULL);
}

int live_bus_present(Live *l) {
    if (l->connected || l->loopback) return 1;
    EdBusPaths p;
    if (!bus_paths(l, &p)) return 0;
    struct stat st;
    if (p.tcp) return stat(p.port, &st) == 0 && stat(p.token, &st) == 0;
    return stat(p.sock, &st) == 0 && stat(p.token, &st) == 0;
}

void live_disconnect(Live *l) {
    if (l->connected) edbus_conn_close(&l->conn);
    edbus_conn_init(&l->conn, -1);
    l->connected = 0;
    l->handshake_done = 0;
}

static const char *scope_name(const unsigned s) {
    return s & EDBUS_SCOPE_EDIT ? "edit" : s & EDBUS_SCOPE_PROJECT ? "project-control" : "read";
}

// Waits for the message answering `id` of type `type` ("res" / "permission_result"); other
// messages (events) are dropped.
static cJSON *wait_for(Live *l, const char *type, const int id, const double timeout_s) {
    const double deadline = now_seconds() + timeout_s;
    while (!l->conn.closed) {
        const double left = deadline - now_seconds();
        if (left <= 0) return NULL;
        cJSON *m = edbus_recv(&l->conn, (int) (left * 1000.0) + 1);
        if (!m) {
            if (l->conn.closed) return NULL;
            continue;
        }
        const char *t = js_str(m, "t", "");
        if (!strcmp(t, type) && js_int(m, "id", -1) == id) return m;
        if (!strcmp(t, "error")) return m;
        cJSON_Delete(m);
    }
    return NULL;
}

static int live_connect(Ctx *ctx, char *err, const size_t errsz) {
    Live *l = &ctx->live;
    EdBusPaths p;
    if (!bus_paths(l, &p)) {
        snprintf(err, errsz, "no editor bus directory (set --bus-dir or KRUNKNATIVE_EDITOR_BUS_DIR)");
        return -1;
    }
    cJSON *welcome = NULL;
    char version[32];
    snprintf(version, sizeof(version), "%s", KN_MCP_VERSION);
    if (!edbus_client_connect(&l->conn, &p, "mcp", "kn_editor_mcp", "kn_editor_mcp", version, 5000, &welcome)) {
        snprintf(err, errsz, "no editor answers on the bus at %s (start one with: client_bin --editor --mcp-bus <maps>)", p.dir);
        cJSON_Delete(welcome);
        edbus_conn_init(&l->conn, -1);
        return -1;
    }
    l->peer = js_int(welcome, "peer", 0);
    cJSON_Delete(welcome);
    l->connected = 1;
    log_msg(LOG_INFO, "live", "attached to the editor bus at %s (peer %d)", p.dir, l->peer);

    // permission handshake
    cJSON *req = edbus_msg("permission_request");
    const int id = l->next_id++;
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddItemToObject(req, "scopes", edbus_scope_json(l->want_scopes));
    cJSON_AddStringToObject(req, "client_id", "kn_editor_mcp");
    cJSON_AddStringToObject(req, "name", "Krunker Editor MCP");
    cJSON_AddStringToObject(req, "version", KN_MCP_VERSION);
    cJSON_AddStringToObject(req, "reason", "An MCP client (e.g. Claude Code) wants to inspect and edit your open maps");
    const int sent = edbus_send(&l->conn, req) && edbus_flush_blocking(&l->conn, 5000) == 1;
    cJSON_Delete(req);
    if (!sent) {
        live_disconnect(l);
        snprintf(err, errsz, "editor bus closed during the permission request");
        return -1;
    }
    log_msg(LOG_INFO, "live", "waiting for the editor's permission dialog (scopes 0x%x)", l->want_scopes);
    cJSON *res = wait_for(l, "permission_result", id, l->permission_timeout_s);
    if (!res) {
        live_disconnect(l);
        snprintf(err, errsz, "the editor did not answer the permission request within %.0f s", l->permission_timeout_s);
        return -1;
    }
    if (!strcmp(js_str(res, "t", ""), "error")) {
        snprintf(err, errsz, "editor bus error: %s", js_str(res, "message", js_str(res, "code", "?")));
        cJSON_Delete(res);
        live_disconnect(l);
        return -1;
    }
    l->granted = edbus_scope_from_json(js_get(res, "granted"));
    snprintf(l->decision, sizeof(l->decision), "%s", js_str(res, "decision", "?"));
    snprintf(l->reason, sizeof(l->reason), "%s", js_str(res, "reason", ""));
    cJSON_Delete(res);
    l->handshake_done = 1;
    if (!l->granted || !strcmp(l->decision, "deny")) {
        l->denied = 1;
        log_msg(LOG_WARN, "live", "the editor denied access (%s%s%s)", l->decision, l->reason[0] ? ": " : "", l->reason);
        snprintf(err, errsz, "the editor denied access (%s%s%s); call editor_connect {retry:true} to ask again", l->decision,
                 l->reason[0] ? ": " : "", l->reason);
        live_disconnect(l);
        return -1;
    }
    log_msg(LOG_INFO, "live", "editor granted 0x%x (%s)", l->granted, l->decision);
    return 0;
}

int live_ensure(Ctx *ctx, const unsigned need, char *err, const size_t errsz) {
    Live *l = &ctx->live;
    if (l->mode == 0) {
        snprintf(err, errsz, "live editor mode is off (--no-live)");
        return -1;
    }
    if (l->loopback) { // in-process: always attached, every scope
        l->connected = l->handshake_done = 1;
        l->granted = EDBUS_SCOPE_ALL;
        snprintf(l->decision, sizeof(l->decision), "embedded");
        return 0;
    }
    if (l->denied) {
        snprintf(err, errsz, "the editor denied access earlier (%s%s%s); call editor_connect {retry:true} to ask again", l->decision,
                 l->reason[0] ? ": " : "", l->reason);
        return -1;
    }
    if (l->connected && l->conn.closed) live_disconnect(l);
    if (!l->connected) {
        if (live_connect(ctx, err, errsz) != 0) {
            snprintf(l->last_error, sizeof(l->last_error), "%s", err);
            return -1;
        }
    }
    if ((l->granted & need) != need) {
        snprintf(err, errsz, "the editor denied %s access (granted:%s%s%s)", scope_name(need & ~l->granted),
                 l->granted & EDBUS_SCOPE_READ ? " read" : "", l->granted & EDBUS_SCOPE_EDIT ? " edit" : "",
                 l->granted & EDBUS_SCOPE_PROJECT ? " project" : "");
        return -1;
    }
    return 0;
}

static unsigned need_for(const char *op, const char *tool) {
    const unsigned s = edbus_scope_for(op, tool);
    return s ? s : EDBUS_SCOPE_READ;
}

cJSON *live_request(Ctx *ctx, const char *op, cJSON *args, const int timeout_ms, char *err, const size_t errsz) {
    const char *tool = op && !strcmp(op, "call") ? js_str(args, "tool", NULL) : NULL;
    if (live_ensure(ctx, need_for(op, tool), err, errsz) != 0) {
        cJSON_Delete(args);
        return NULL;
    }
    Live *l = &ctx->live;
    if (l->loopback) {
        cJSON *res = l->loopback(op, args, err, errsz);
        cJSON_Delete(args);
        return res;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        const int id = l->next_id++;
        cJSON *req = edbus_req(id, op, attempt == 0 ? args : cJSON_Duplicate(args, 1));
        if (attempt == 0) args = cJSON_Duplicate(args, 1); // keep a copy for a retry
        const int ok = edbus_send(&l->conn, req) && edbus_flush_blocking(&l->conn, 10000) == 1;
        cJSON_Delete(req);
        cJSON *res = ok ? wait_for(l, "res", id, timeout_ms / 1000.0) : NULL;
        if (!res && l->conn.closed && attempt == 0) {
            // host went away: reconnect once (editors take the bus over within ~3 s)
            live_disconnect(l);
            usleep(1500 * 1000);
            if (live_ensure(ctx, need_for(op, tool), err, errsz) != 0) {
                cJSON_Delete(args);
                return NULL;
            }
            continue;
        }
        cJSON_Delete(args);
        if (!res) {
            snprintf(err, errsz, "editor did not answer '%s' within %d ms", op, timeout_ms);
            return NULL;
        }
        if (!strcmp(js_str(res, "t", ""), "error")) {
            snprintf(err, errsz, "editor bus error: %s", js_str(res, "message", "?"));
            cJSON_Delete(res);
            return NULL;
        }
        if (!js_bool(res, "ok", 0)) {
            const cJSON *e = js_get(res, "error");
            const char *code = js_str(e, "code", "error");
            if (!strcmp(code, "permission_denied"))
                snprintf(err, errsz, "the editor denied %s access: %s", scope_name(need_for(op, tool)), js_str(e, "message", ""));
            else snprintf(err, errsz, "editor: %s (%s)", js_str(e, "message", "request failed"), code);
            cJSON_Delete(res);
            return NULL;
        }
        cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(res, "result");
        cJSON_Delete(res);
        return result ? result : cJSON_CreateObject();
    }
    cJSON_Delete(args);
    snprintf(err, errsz, "editor bus unavailable");
    return NULL;
}

cJSON *live_call(Ctx *ctx, const char *project, const char *tool, cJSON *args, const int timeout_ms, char *err, const size_t errsz) {
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "project", project);
    cJSON_AddStringToObject(a, "tool", tool);
    cJSON_AddItemToObject(a, "args", args ? args : cJSON_CreateObject());
    return live_request(ctx, "call", a, timeout_ms, err, errsz);
}

cJSON *live_status_json(Ctx *ctx) {
    Live *l = &ctx->live;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mode", l->mode == 0 ? "off" : l->mode == 1 ? "auto" : "required");
    cJSON_AddBoolToObject(o, "bus_present", l->mode ? live_bus_present(l) : 0);
    cJSON_AddBoolToObject(o, "attached", l->loopback || (l->connected && !l->conn.closed));
    if (l->loopback) cJSON_AddBoolToObject(o, "embedded", 1);
    cJSON_AddItemToObject(o, "requested", edbus_scope_json(l->want_scopes));
    cJSON_AddItemToObject(o, "granted", edbus_scope_json(l->granted));
    if (l->decision[0]) cJSON_AddStringToObject(o, "decision", l->decision);
    if (l->reason[0]) cJSON_AddStringToObject(o, "reason", l->reason);
    cJSON_AddBoolToObject(o, "denied", l->denied);
    if (l->last_error[0]) cJSON_AddStringToObject(o, "last_error", l->last_error);
    EdBusPaths p;
    if (edbus_paths_init(&p, l->bus_dir[0] ? l->bus_dir : NULL)) cJSON_AddStringToObject(o, "bus_dir", p.dir);
    return o;
}
