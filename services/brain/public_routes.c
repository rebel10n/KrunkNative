/*
 * services/brain/public_routes.c - public matchmaker HTTP routes (v3.1.1
 * matchmaker/publicRoutes.ts, client libs/ClientManager 99.js). Shapes and
 * error names: doc/backend.md section 4. Every route is registered at
 * /mm/<x> and /<x>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "brain_internal.h"
#include "releases.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

/* ---- helpers */

static void reply_error(kn_http_req *req, int status, const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 0);
    cJSON_AddStringToObject(o, "error", name);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static int error_status(const char *name) {
    if (!strcmp(name, "InvalidGameId")) return 404;
    if (!strcmp(name, "GameFull")) return 409;
    if (!strcmp(name, "BadRequest")) return 400;
    if (!strcmp(name, "AccountRequired")) return 403;
    if (!strcmp(name, "IncompatibleVersion")) return 409;
    return 503;
}

static int query_bool(kn_http_req *req, const char *key, int def) {
    char v[16];
    if (kn_http_query_get(req, key, v, sizeof(v)) < 0 || !v[0]) return def;
    return !(strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0 || strcasecmp(v, "no") == 0);
}

/* FORCE_ENTER=1 cookie (publicRoutes.ts hasForceEnterCookie) */
static int has_force_enter(kn_http_req *req) {
    const char *c = kn_http_header_get(req, "Cookie");
    if (!c) return 0;
    const char *p = c;
    while ((p = strstr(p, "FORCE_ENTER")) != NULL) {
        int at_start = p == c || p[-1] == ' ' || p[-1] == ';';
        const char *q = p + strlen("FORCE_ENTER");
        while (*q == ' ') q++;
        if (at_start && *q == '=') {
            q++;
            while (*q == ' ') q++;
            if (q[0] == '1' && (q[1] == 0 || q[1] == ';' || q[1] == ' ')) return 1;
        }
        p = q;
    }
    return 0;
}

/* publicRoutes.ts switches on the path only: every method gets the same answer. */
static void route2(Brain *b, const char *method, const char *name, kn_http_handler h) {
    char path[96];
    snprintf(path, sizeof(path), "/mm/%s", name);
    kn_http_route(b->http, method, path, h, b);
    snprintf(path, sizeof(path), "/%s", name);
    kn_http_route(b->http, method, path, h, b);
}

/* "http://h:p/x" -> "h:p", without the port when it is 443 (registry.ts regions()).
 * ClientManager.getRegion() then fetches http://<value>/ping when the value ends
 * with ":6767", https://<value>/ping otherwise. */
static void url_to_ping_host(const char *url, char *out, size_t cap) {
    kn_url u;
    if (kn_url_parse(url, &u) == 0) {
        if (u.port && u.port != 443) snprintf(out, cap, "%s:%d", u.host, u.port);
        else snprintf(out, cap, "%s", u.host);
        return;
    }
    snprintf(out, cap, "%s", url);
}

static const cJSON *region_conf(const char *id) {
    const cJSON *regions = mm_settings()->regions, *r;
    cJSON_ArrayForEach(r, regions) {
        if (!strcmp(kn_json_str(r, "id", ""), id)) return r;
    }
    return NULL;
}

/* ---- /ping */

static void h_ping(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "success", "true");
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

/* ---- /mm/ping-list : {region: "host[:port]"} (registry.ts regions()) */

static void h_ping_list(kn_http_req *req, void *ud) {
    Brain *b = ud;
    Registry *r = b->registry;
    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; i < r->node_count; i++) {
        const MmNode *n = r->nodes[i];
        if (!n->region[0] || cJSON_GetObjectItemCaseSensitive(o, n->region)) continue;
        char host[300] = "";
        const cJSON *rc = region_conf(n->region);
        const char *ping = kn_json_str(rc, "ping_host", NULL);
        if (ping && *ping) {
            snprintf(host, sizeof(host), "%s", ping);
        } else {
            const char *url = mm_settings()->public_url;
            for (size_t k = 0; k < r->brain_count; k++) {
                if (!strcmp(r->brains[k].brain_id, n->brain_id) && r->brains[k].public_url[0])
                    url = r->brains[k].public_url;
            }
            url_to_ping_host(url, host, sizeof(host));
        }
        cJSON_AddStringToObject(o, n->region, host);
    }
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

/* ---- /mm/regions (KrunkNative) */

static cJSON *region_entry(cJSON *arr, const char *id, const char *name) {
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!strcmp(kn_json_str(it, "id", ""), id)) return it;
    }
    it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "id", id);
    cJSON_AddStringToObject(it, "name", name ? name : id);
    cJSON_AddNumberToObject(it, "players", 0);
    cJSON_AddNumberToObject(it, "games", 0);
    cJSON_AddNumberToObject(it, "nodes", 0);
    cJSON_AddItemToArray(arr, it);
    return it;
}

static void bump(cJSON *o, const char *key, int by) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (v) cJSON_SetNumberValue(v, v->valuedouble + by);
}

static void h_regions(kn_http_req *req, void *ud) {
    Brain *b = ud;
    cJSON *arr = cJSON_CreateArray();
    const cJSON *rc;
    cJSON_ArrayForEach(rc, mm_settings()->regions) {
        region_entry(arr, kn_json_str(rc, "id", "?"), kn_json_str(rc, "name", NULL));
    }
    Registry *r = b->registry;
    for (size_t i = 0; i < r->node_count; i++) {
        const MmNode *n = r->nodes[i];
        cJSON *e = region_entry(arr, n->region, kn_json_str(region_conf(n->region), "name", NULL));
        bump(e, "nodes", 1);
        bump(e, "games", (int) n->lobby_count);
        for (size_t j = 0; j < n->lobby_count; j++) bump(e, "players", n->lobbies[j].players);
    }
    brain_reply_json(req, 200, arr);
    cJSON_Delete(arr);
}

/* ---- /mm/game-list */

static void h_game_list(kn_http_req *req, void *ud) {
    Brain *b = ud;
    if (b->draining) {
        reply_error(req, 503, "updating");
        return;
    }
    int total = 0;
    cJSON *games = registry_game_list(b->registry, kn_wall_ms(), &total);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    cJSON_AddNumberToObject(o, "totalPlayerCount", total);
    cJSON_AddItemToObject(o, "games", games);
    /* meta.outdated / meta.incompatible + latest, download_url, downloads (releases.h) */
    char proto[16] = "";
    kn_http_query_get(req, "proto", proto, sizeof(proto));
    releases_annotate_tuples(o, games, releases_client_major(proto));
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

/* ---- /mm/game-info?game=ID
 * publicRoutes.ts: the requested lobby whatever its state, else pickBestLobby()
 * without the requested id: the most populated public lobby with room, else the
 * least populated public lobby (any lobby when there is no public one); 503 when
 * there is no lobby at all. Reported players only (it is informational: the
 * client shows FULL / n/max), every region. */
static MmLobby *game_info_pick(Registry *r) {
    MmLobby *best = NULL, *least = NULL, *least_any = NULL;
    for (size_t i = 0; i < r->node_count; i++) {
        MmNode *n = r->nodes[i];
        for (size_t j = 0; j < n->lobby_count; j++) {
            MmLobby *l = &n->lobbies[j];
            if (!least_any || l->players < least_any->players) least_any = l;
            if (l->custom) continue;
            if (!least || l->players < least->players) least = l;
            if (l->players < l->max_players && (!best || l->players > best->players)) best = l;
        }
    }
    return best ? best : least ? least : least_any;
}

static void h_game_info(kn_http_req *req, void *ud) {
    Brain *b = ud;
    char game[MM_GAME_ID_MAX] = "";
    kn_http_query_get(req, "game", game, sizeof(game));
    MmLobby *l = game[0] ? registry_lobby(b->registry, game, NULL) : NULL;
    if (!l) l = game_info_pick(b->registry);
    if (!l) {
        reply_error(req, 503, "NoAvailableServers");
        return;
    }
    cJSON *t = registry_lobby_tuple(b->registry, l, kn_wall_ms());
    brain_reply_json(req, 200, t);
    cJSON_Delete(t);
}

/* ---- session token -> account (svc-accounts "token" op), shared by seek/host */

typedef void (*AccountCb)(kn_http_req *req, Brain *b, int64_t account_id, const char *name, void *ctx);

typedef struct AccountLookup {
    kn_http_req *req;
    Brain *b;
    AccountCb cb;
    void *ctx;
} AccountLookup;

static void on_account(void *ud, const char *err, const cJSON *data) {
    AccountLookup *a = ud;
    int64_t id = 0;
    const char *name = NULL;
    if (!err && data) {
        id = kn_json_int(data, "id", 0);
        name = kn_json_str(data, "username", kn_json_str(data, "name", NULL));
    }
    if (err) KN_DEBUG("routes", "session lookup failed: %s (joining as guest)", err);
    a->cb(a->req, a->b, id, name, a->ctx);
    free(a);
}

static void with_account(kn_http_req *req, Brain *b, AccountCb cb, void *ctx) {
    char token[512] = "";
    if (kn_http_query_get(req, "token", token, sizeof(token)) <= 0) {
        const char *bearer = kn_http_bearer(req);
        if (bearer) snprintf(token, sizeof(token), "%s", bearer);
    }
    if (!token[0]) {
        cb(req, b, 0, NULL, ctx);
        return;
    }
    AccountLookup *a = calloc(1, sizeof(*a));
    if (!a) {
        cb(req, b, 0, NULL, ctx);
        return;
    }
    a->req = req;
    a->b = b;
    a->cb = cb;
    a->ctx = ctx;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(b, "token", args, on_account, a);
    cJSON_Delete(args);
}

/* ---- /mm/seek-game */

static void seek_with_account(kn_http_req *req, Brain *b, int64_t account_id, const char *name, void *ctx) {
    (void) ctx;
    if (!kn_http_req_alive(req)) {
        kn_http_respond_error(req, 499, "gone");
        return;
    }
    char region[64] = "", game[MM_GAME_ID_MAX] = "", dq[512] = "", version[32] = "", proto[16] = "";
    kn_http_query_get(req, "region", region, sizeof(region));
    kn_http_query_get(req, "proto", proto, sizeof(proto)); /* the client's protocol "M.m" */
    kn_http_query_get(req, "game", game, sizeof(game));
    if (kn_http_query_get(req, "dataQuery", dq, sizeof(dq)) > 0 && mm_settings()->policy.require_version) {
        cJSON *q = kn_json_parse(dq, strlen(dq));
        snprintf(version, sizeof(version), "%s", kn_json_str(q, "v", ""));
        cJSON_Delete(q);
    }
    MmPickQuery q = {
        .region = region,
        .game_id = game[0] ? game : NULL,
        .auto_change = query_bool(req, "autoChangeGame", 1),
        .force = has_force_enter(req) || query_bool(req, "force", 0),
        .version = version[0] ? version : NULL,
        .authed = account_id > 0, /* a valid session: online_mode lobbies verify accounts */
        .now_ms = kn_wall_ms(),
        /* never a lobby of another protocol major; up-to-date lobbies first (releases.h).
         * Clients from before ?proto= are 1.x like every server so far. */
        .proto_major = proto[0] ? releases_client_major(proto) : 1,
        .outdated = releases_lobby_outdated,
    };
    MmPick pick;
    MmPickError e = registry_pick(b->registry, &mm_settings()->policy, &q, &pick);
    if (e != MM_PICK_OK) {
        static const char *names[] = {"", "InvalidGameId", "GameFull", "NoServersMatchQuery", "NoAvailableServers",
                                      "AccountRequired", "IncompatibleVersion"};
        reply_error(req, error_status(names[e]), names[e]);
        return;
    }
    cJSON *reply = brain_seek_reply(b, pick.node, pick.lobby->game_id, account_id, name, pick.changed);
    if (!reply) {
        reply_error(req, 500, "internal");
        return;
    }
    KN_DEBUG("routes", "seek region=%s game=%s %s -> %s (%s:%d%s)", region, game, account_id > 0 ? "account" : "guest",
             pick.lobby->game_id, pick.node->public_host, pick.node->public_port,
             registry_lobby_needs_account(pick.node, pick.lobby) ? ", online_mode" : "");
    brain_reply_json(req, 200, reply);
    cJSON_Delete(reply);
}

/* A CORS preflight (the reference answers it like a GET) must not issue a ticket
 * nor reserve a seat: 204 with the cors.conf headers. */
static int preflight(kn_http_req *req) {
    if (strcmp(req->method, "OPTIONS") != 0) return 0;
    brain_cors_headers(req);
    kn_http_respond(req, 204, NULL, NULL, 0);
    return 1;
}

static void h_seek_game(kn_http_req *req, void *ud) {
    Brain *b = ud;
    if (preflight(req)) return;
    if (b->draining) {
        reply_error(req, 503, "updating");
        return;
    }
    with_account(req, b, seek_with_account, NULL);
}

/* ---- /mm/host-game */

typedef struct HostReq {
    kn_http_req *req;
    char region[64];
    cJSON *config;
} HostReq;

static void on_hosted(void *ud, const char *err, const cJSON *reply) {
    HostReq *h = ud;
    if (err) reply_error(h->req, error_status(err), err);
    else brain_reply_json(h->req, 200, reply);
    cJSON_Delete(h->config);
    free(h);
}

static void host_with_account(kn_http_req *req, Brain *b, int64_t account_id, const char *name, void *ctx) {
    HostReq *h = ctx;
    brain_host_game(b, h->region, h->config, account_id, name, on_hosted, h);
}

static void h_host_game(kn_http_req *req, void *ud) {
    Brain *b = ud;
    if (preflight(req)) return;
    if (b->draining) {
        reply_error(req, 503, "updating");
        return;
    }
    HostReq *h = calloc(1, sizeof(*h));
    if (!h) {
        reply_error(req, 500, "internal");
        return;
    }
    h->req = req;
    cJSON *body = req->body_len ? kn_http_body_json(req) : NULL;
    if (body) {
        snprintf(h->region, sizeof(h->region), "%s", kn_json_str(body, "region", ""));
        cJSON *cfg = cJSON_DetachItemFromObjectCaseSensitive(body, "config");
        h->config = cfg;
        cJSON_Delete(body);
    }
    if (!h->region[0]) kn_http_query_get(req, "region", h->region, sizeof(h->region));
    if (!h->config) {
        size_t cap = strlen(req->query) + 1; /* a decoded value is never longer than the raw query */
        char *cfg = malloc(cap);
        if (cfg && kn_http_query_get(req, "config", cfg, cap) > 0) h->config = kn_json_parse(cfg, strlen(cfg));
        free(cfg);
    }
    if (!h->config) h->config = cJSON_CreateObject();
    if (!cJSON_IsObject(h->config)) {
        cJSON_Delete(h->config);
        free(h);
        reply_error(req, 400, "BadRequest");
        return;
    }
    with_account(req, b, host_with_account, h);
}

/* ---- player counts, status, brains */

static void h_player_count(kn_http_req *req, void *ud) {
    Brain *b = ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "playerCount", registry_player_count(b));
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

static void h_status(kn_http_req *req, void *ud) {
    Brain *b = ud;
    Registry *r = b->registry;
    int lobbies = 0, local = 0, online = 0;
    for (size_t i = 0; i < r->node_count; i++) {
        const MmNode *n = r->nodes[i];
        lobbies += (int) n->lobby_count;
        local += n->local;
        int om = n->online_mode;
        for (size_t j = 0; j < n->lobby_count && !om; j++) om = n->lobbies[j].online_mode;
        online += om;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "brain", b->id);
    cJSON_AddStringToObject(o, "region", b->region);
    cJSON_AddStringToObject(o, "version", b->version);
    cJSON_AddBoolToObject(o, "federation", federation_active());
    cJSON_AddNumberToObject(o, "nodes", (double) r->node_count);
    cJSON_AddNumberToObject(o, "localNodes", local);
    cJSON_AddNumberToObject(o, "onlineModeNodes", online);
    cJSON_AddNumberToObject(o, "lobbies", lobbies);
    cJSON_AddNumberToObject(o, "players", registry_player_count(b));
    cJSON_AddNumberToObject(o, "uptimeMs", (double) (kn_wall_ms() - b->started_ms));
    cJSON_AddBoolToObject(o, "draining", b->draining);
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

static void h_brains(kn_http_req *req, void *ud) {
    Brain *b = ud;
    Registry *r = b->registry;
    cJSON *arr = cJSON_CreateArray();
    int self_listed = 0;
    for (size_t k = 0; k < r->brain_count; k++) {
        const MmBrainInfo *bi = &r->brains[k];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", bi->brain_id);
        cJSON_AddStringToObject(o, "region", bi->region);
        cJSON_AddStringToObject(o, "publicUrl", bi->public_url);
        int nodes = 0;
        for (size_t i = 0; i < r->node_count; i++) nodes += !strcmp(r->nodes[i]->brain_id, bi->brain_id);
        cJSON_AddNumberToObject(o, "nodes", nodes);
        cJSON_AddNumberToObject(o, "lastSeen", (double) bi->heartbeat_ms);
        cJSON_AddBoolToObject(o, "self", !strcmp(bi->brain_id, b->id));
        self_listed |= !strcmp(bi->brain_id, b->id);
        cJSON_AddItemToArray(arr, o);
    }
    if (!self_listed) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", b->id);
        cJSON_AddStringToObject(o, "region", b->region);
        cJSON_AddStringToObject(o, "publicUrl", mm_settings()->public_url);
        cJSON_AddNumberToObject(o, "nodes", internal_ws_node_count());
        cJSON_AddNumberToObject(o, "lastSeen", (double) kn_wall_ms());
        cJSON_AddBoolToObject(o, "self", 1);
        cJSON_AddItemToArray(arr, o);
    }
    brain_reply_json(req, 200, arr);
    cJSON_Delete(arr);
}

/* ---- admin: POST /mm/admin/drain?node=ID (Bearer brain.internal_secret) */

static void h_admin_drain(kn_http_req *req, void *ud) {
    Brain *b = ud;
    const char *bearer = kn_http_bearer(req);
    if (!bearer || !kn_ct_streq(bearer, mm_settings()->internal_secret)) {
        reply_error(req, 401, "unauthorized");
        return;
    }
    char node[MM_ID_MAX] = "", reason[128] = "admin";
    kn_http_query_get(req, "node", node, sizeof(node));
    kn_http_query_get(req, "reason", reason, sizeof(reason));
    MmNode *n = registry_node(b->registry, node);
    if (!n || !n->local) {
        reply_error(req, 404, "NodeNotLocal");
        return;
    }
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", "drain");
    cJSON_AddStringToObject(f, "reason", reason);
    internal_ws_send(b, node, f);
    cJSON_Delete(f);
    n->draining = 1;
    federation_node_changed(b, n);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    cJSON_AddStringToObject(o, "node", node);
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

/* coord-auth: admin rcon relay. POST /mm/admin/rcon?node=<id> {command} (Bearer
 * brain.internal_secret) -> internal WS {"type":"rcon","rid","command"} to that game server
 * -> {"type":"rcon-reply","rid","ok","output"} -> {success, node, output}. Local nodes only
 * (a node connected to another brain: ask that brain). */
typedef struct RconReq {
    kn_http_req *req;
    char node[MM_ID_MAX];
} RconReq;

static void on_rcon_reply(void *ud, const cJSON *reply, const char *err) {
    RconReq *r = ud;
    if (!reply) {
        reply_error(r->req, 504, err ? err : "timeout");
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", kn_json_bool(reply, "ok", 0));
        cJSON_AddStringToObject(o, "node", r->node);
        cJSON_AddStringToObject(o, "output", kn_json_str(reply, "output", ""));
        brain_reply_json(r->req, 200, o);
        cJSON_Delete(o);
    }
    free(r);
}

static void h_admin_rcon(kn_http_req *req, void *ud) {
    Brain *b = ud;
    const char *bearer = kn_http_bearer(req);
    if (!bearer || !kn_ct_streq(bearer, mm_settings()->internal_secret)) {
        reply_error(req, 401, "unauthorized");
        return;
    }
    char node[MM_ID_MAX] = "", command[512] = "";
    kn_http_query_get(req, "node", node, sizeof(node));
    cJSON *body = kn_http_body_json(req);
    snprintf(command, sizeof(command), "%s", kn_json_str(body, "command", ""));
    if (!node[0]) snprintf(node, sizeof(node), "%s", kn_json_str(body, "node", ""));
    cJSON_Delete(body);
    if (!command[0]) kn_http_query_get(req, "command", command, sizeof(command));
    if (!node[0] || !command[0]) {
        reply_error(req, 400, "BadRequest");
        return;
    }
    RconReq *r = calloc(1, sizeof(*r));
    if (!r) {
        reply_error(req, 503, "NoMemory");
        return;
    }
    r->req = req;
    snprintf(r->node, sizeof(r->node), "%s", node);
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", "rcon");
    cJSON_AddStringToObject(f, "command", command);
    KN_INFO("routes", "admin rcon -> %s: %s", node, command);
    if (internal_ws_request(b, node, f, 10000, on_rcon_reply, r) != 0) {
        free(r);
        reply_error(req, 404, "NodeNotLocal");
    }
}

/* index.ts: anything no route matched (the brain has no static site here):
 * 404 {"error":"not_found","path":req.url}, bare JSON content type. */
static void h_not_found(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", "not_found");
    cJSON_AddStringToObject(o, "path", req->target);
    brain_cors_headers(req);
    brain_reply_json_raw(req, 404, o);
    cJSON_Delete(o);
}

int public_routes_init(Brain *b) {
    /* method NULL: publicRoutes.ts answers every method alike (doc/backend.md 4.1) */
    kn_http_route(b->http, NULL, "/ping", h_ping, b);
    kn_http_route(b->http, NULL, "/mm/ping", h_ping, b);
    route2(b, NULL, "ping-list", h_ping_list);
    route2(b, NULL, "regions", h_regions);
    route2(b, NULL, "game-list", h_game_list);
    route2(b, NULL, "game-info", h_game_info);
    route2(b, NULL, "seek-game", h_seek_game);
    route2(b, NULL, "host-game", h_host_game); /* GET ?config= or POST {region, config} */
    route2(b, NULL, "status", h_status);
    route2(b, NULL, "brains", h_brains);
    kn_http_route(b->http, "POST", "/mm/admin/drain", h_admin_drain, b);
    kn_http_route(b->http, "POST", "/mm/admin/rcon", h_admin_rcon, b);
    kn_http_route(b->http, NULL, "/api/player-count", h_player_count, b);
    kn_http_route(b->http, NULL, "/social/player-count", h_player_count, b);
    kn_http_route(b->http, NULL, "/mm/player-count", h_player_count, b);
    kn_http_set_fallback(b->http, h_not_found, b);
    return 0;
}
