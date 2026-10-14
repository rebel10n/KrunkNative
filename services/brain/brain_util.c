/*
 * services/brain/brain_util.c - brain settings (brain.* config), join tickets,
 * seek-game reply builder, JSON reply helper and the weak accounts fallback.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_internal.h"
#include "auth_state.h" /* AuthLimiter */
#include "hot.h"        /* hot_limit */
#include "mm_token.h"   /* seek-game validation tokens */
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

static MmSettings g_mm;

static void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

static int clamp_int(int64_t v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : (int) v;
}

int mm_settings_load(Brain *b) {
    const struct kn_config *cfg = b->cfg;
    copy_str(g_mm.internal_secret, sizeof(g_mm.internal_secret),
             kn_config_str(cfg, "brain.internal_secret", "kn-dev-secret"));
    copy_str(g_mm.join_secret, sizeof(g_mm.join_secret),
             kn_config_str(cfg, "brain.join_secret", g_mm.internal_secret));
    if (!g_mm.join_secret[0]) copy_str(g_mm.join_secret, sizeof(g_mm.join_secret), g_mm.internal_secret);
    if (!strcmp(g_mm.internal_secret, "kn-dev-secret"))
        KN_WARN("brain", "brain.internal_secret is the development default; set it for deployments");
    g_mm.join_ttl_s = clamp_int(kn_config_int(cfg, "brain.join_ttl_s", 30), 5, 3600);
    /* the seek-game validation token (mm_token.h, doc/v10/matchmaker-token.md) */
    const MmTokenSettings token = {
        .secret = kn_config_str(cfg, "brain.validation_secret", ""),
        .join_secret = g_mm.join_secret,
        .ttl_s = (int) kn_config_int(cfg, "brain.matchmaking.validation_ttl_s", 60),
        .bind_ip = kn_config_bool(cfg, "brain.matchmaking.validation_bind_ip", 0),
        .require = kn_config_bool(cfg, "brain.matchmaking.require_validation_token", 0),
    };
    if (mm_token_init(&token) != 0) KN_WARN("brain", "no key for seek-game validation tokens (brain.join_secret is blank)");
    else if (!token.require) KN_INFO("brain", "seek-game validation tokens: optional (brain.matchmaking.require_validation_token off)");
    g_mm.node_ttl_s = clamp_int(kn_config_int(cfg, "brain.node_ttl_s", 30), 3, 3600);
    g_mm.heartbeat_s = clamp_int(kn_config_int(cfg, "brain.heartbeat_s", 5), 1, 600);
    if (g_mm.heartbeat_s * 2 > g_mm.node_ttl_s) g_mm.heartbeat_s = g_mm.node_ttl_s / 2 > 0 ? g_mm.node_ttl_s / 2 : 1;
    g_mm.ping_interval_s = clamp_int(kn_config_int(cfg, "brain.ping_interval_s", 10), 1, 600);
    g_mm.host_timeout_s = clamp_int(kn_config_int(cfg, "brain.host_timeout_s", 10), 1, 120);
    g_mm.federation = kn_config_bool(cfg, "brain.federation", 1) && b->db;
    g_mm.policy.cross_region = kn_config_bool(cfg, "brain.matchmaking.cross_region", 1);
    g_mm.policy.default_max_players = clamp_int(kn_config_int(cfg, "brain.matchmaking.max_players", 8), 1, 1000);
    g_mm.policy.soft_cap = clamp_int(kn_config_int(cfg, "brain.matchmaking.soft_cap", 0), 0, 1000);
    g_mm.policy.require_version = kn_config_bool(cfg, "brain.matchmaking.require_version", 0);
    const cJSON *caps = kn_config_get(cfg, "brain.matchmaking.mode_caps");
    g_mm.policy.mode_caps = cJSON_IsObject(caps) ? caps : NULL;
    const cJSON *regions = kn_config_get(cfg, "brain.regions");
    g_mm.regions = cJSON_IsArray(regions) ? regions : NULL;
    char def_url[256];
    snprintf(def_url, sizeof(def_url), "http://%s:%d",
             kn_config_str(cfg, "brain.host", "127.0.0.1"), (int) kn_config_int(cfg, "brain.port", 5050));
    copy_str(g_mm.public_url, sizeof(g_mm.public_url), kn_config_str(cfg, "brain.public_url", def_url));
    /* brain.cors_origins (main.c): absent = ["*"] */
    g_mm.cors_any = 1;
    if (kn_config_has(cfg, "brain.cors_origins")) {
        const char *origins[32];
        size_t n = kn_config_str_list(cfg, "brain.cors_origins", origins, 32);
        g_mm.cors_any = 0;
        for (size_t i = 0; i < n && i < 32; i++) g_mm.cors_any |= !strcmp(origins[i], "*");
    }
    return 0;
}

const MmSettings *mm_settings(void) {
    return &g_mm;
}

/* The CORS headers the v3.1.1 deployment's nginx adds to every brain response
 * ("always", ansible/files/nginx/cors.conf). Access-Control-Allow-Origin comes from
 * knsvc (brain.cors_origins) when the request carries an Origin; without one the
 * reference still sends "*", so add it here for a wildcard configuration. */
void brain_cors_headers(struct kn_http_req *req) {
    if (g_mm.cors_any && !kn_http_header_get(req, "Origin")) kn_http_set_header(req, "Access-Control-Allow-Origin", "*");
    kn_http_set_header(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS, PUT, DELETE, PATCH");
    kn_http_set_header(req, "Access-Control-Allow-Headers",
                       "Authorization, Content-Type, X-Requested-With, Accept, Origin, Cache-Control, Range, If-Modified-Since");
    kn_http_set_header(req, "Access-Control-Expose-Headers", "Content-Length, Content-Range");
    kn_http_set_header(req, "Access-Control-Max-Age", "86400");
}

/* JSON.stringify body with "Content-Type: application/json" (no charset), like
 * publicRoutes.ts writeJson / index.ts's 404. */
void brain_reply_json_raw(struct kn_http_req *req, int status, const cJSON *json) {
    char *s = json ? cJSON_PrintUnformatted(json) : NULL;
    if (!s) {
        kn_http_respond(req, 500, "application/json", "{\"error\":\"internal\"}", 20);
        return;
    }
    kn_http_respond(req, status, "application/json", s, strlen(s));
    free(s);
}

void brain_reply_json(struct kn_http_req *req, int status, const cJSON *json) {
    /* v3.1.1 publicRoutes.ts JSON_HEADERS (+ the nginx CORS headers) */
    brain_cors_headers(req);
    kn_http_set_header(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    kn_http_set_header(req, "Pragma", "no-cache");
    kn_http_set_header(req, "Expires", "0");
    brain_reply_json_raw(req, status, json);
}

/* NetJoinPacket.client_key (shared/include/shared.h, char[352]) minus the NUL */
#define BRAIN_TICKET_MAX 351

cJSON *brain_seek_reply(Brain *b, MmNode *n, const char *game_id, int64_t account_id,
                        const char *account_name, int changed) {
    return brain_seek_reply_ex(b, n, game_id, account_id, account_name, changed, 1);
}

cJSON *brain_seek_reply_ex(Brain *b, MmNode *n, const char *game_id, int64_t account_id,
                           const char *account_name, int changed, int reserve) {
    const MmSettings *s = mm_settings();
    char jti[40];
    if (kn_random_hex(jti, sizeof(jti), 16)) return NULL;
    const char *colon = strchr(game_id, ':');
    /* Only what the game server / the brain's "auth" read: the ticket travels in
     * NetJoinPacket.client_key (char[352]), and an account ticket with iss/gid/reg overflowed
     * it from ~7-char usernames on ("Invalid join key"). gid = nid:lid. */
    cJSON *claims = cJSON_CreateObject();
    cJSON_AddStringToObject(claims, "brn", b->id);
    cJSON_AddStringToObject(claims, "nid", n->node_id);
    cJSON_AddStringToObject(claims, "lid", colon ? colon + 1 : game_id);
    cJSON_AddNumberToObject(claims, "acc", (double) (account_id > 0 ? account_id : 0));
    if (account_id > 0 && account_name && *account_name) cJSON_AddStringToObject(claims, "name", account_name);
    cJSON_AddStringToObject(claims, "jti", jti);
    char *ticket = kn_signed_token_create(s->join_secret, claims, s->join_ttl_s);
    if (ticket && strlen(ticket) > BRAIN_TICKET_MAX) {
        /* still too long (long node id / name): the server takes the name from the
         * brain's ticket auth instead */
        free(ticket);
        cJSON_DeleteItemFromObjectCaseSensitive(claims, "name");
        ticket = kn_signed_token_create(s->join_secret, claims, s->join_ttl_s);
        if (ticket && strlen(ticket) > BRAIN_TICKET_MAX) {
            KN_ERROR("routes", "join ticket for %s is %zu chars (max %d)", game_id, strlen(ticket), BRAIN_TICKET_MAX);
            free(ticket);
            ticket = NULL;
        }
    }
    cJSON_Delete(claims);
    if (!ticket) return NULL;

    if (reserve) {
        int64_t now = kn_wall_ms();
        registry_reserve(b->registry, jti, game_id, now + (int64_t) s->join_ttl_s * 1000);
        federation_reserve(b, jti, game_id, account_id, s->join_ttl_s);
    }

    /* best-effort heads-up to the node (v3.1.1 has none: the ticket is authoritative) */
    cJSON *assign = cJSON_CreateObject();
    cJSON_AddStringToObject(assign, "type", "assign");
    cJSON_AddStringToObject(assign, "ticket", ticket);
    cJSON_AddStringToObject(assign, "jti", jti);
    cJSON_AddStringToObject(assign, "lobbyId", colon ? colon + 1 : game_id);
    cJSON_AddStringToObject(assign, "gameId", game_id);
    if (account_id > 0) {
        cJSON *acc = cJSON_AddObjectToObject(assign, "account");
        cJSON_AddNumberToObject(acc, "id", (double) account_id);
        cJSON_AddStringToObject(acc, "name", account_name ? account_name : "");
    } else {
        cJSON_AddNullToObject(assign, "account");
    }
    cJSON_AddNumberToObject(assign, "expiresIn", s->join_ttl_s);
    if (n->local) internal_ws_send(b, n->node_id, assign);
    else federation_assign_remote(b, n, assign);
    cJSON_Delete(assign);

    cJSON *reply = cJSON_CreateObject();
    if (changed) cJSON_AddStringToObject(reply, "changeReason", "game_changed");
    else cJSON_AddNullToObject(reply, "changeReason");
    cJSON_AddStringToObject(reply, "gameId", game_id);
    cJSON_AddStringToObject(reply, "lobbyId", game_id);
    cJSON_AddStringToObject(reply, "host", n->public_host);
    cJSON_AddNumberToObject(reply, "port", n->public_port);
    cJSON_AddStringToObject(reply, "clientId", ticket);
    cJSON_AddStringToObject(reply, "region", n->region);
    if (n->web_url[0]) cJSON_AddStringToObject(reply, "webUrl", n->web_url);
    else cJSON_AddNullToObject(reply, "webUrl");
    free(ticket);
    return reply;
}

/* Weak fallback until/unless svc-accounts' accounts.c is linked in. */
__attribute__((weak)) int accounts_internal_call(Brain *b, const char *op, const cJSON *args,
                                                 AcctReplyFn cb, void *ud) {
    (void) b;
    (void) op;
    (void) args;
    if (cb) cb(ud, "accounts unavailable", NULL);
    return 0;
}

/* ---- the internal secret gate (mm_internal_authorized, brain_internal.h) */

#define MM_INTERNAL_TRIES_PER_MIN 30 /* per address: game servers connect once, admins rarely */
#define MM_INTERNAL_MIN_SECRET 16

static int mm_addr_is_loopback(const char *a) {
    return a && (!strncmp(a, "127.", 4) || !strcmp(a, "::1") || !strncmp(a, "::ffff:127.", 11));
}

int mm_internal_authorized(kn_http_req *req, const char *what) {
    static AuthLimiter *tries;
    if (!tries) tries = auth_limiter_new(MM_INTERNAL_TRIES_PER_MIN, 60000);
    const char *ip = req->remote_addr ? req->remote_addr : "";
    /* every attempt counts (the per-process limiter, then the cross-brain one in Redis): the
     * secret can't be guessed at network speed, and refusals stay cheap */
    if (!hot_limit("internal_auth", ip, MM_INTERNAL_TRIES_PER_MIN, 60, auth_limiter_hit(tries, ip, kn_now_ms()))) {
        KN_WARN("internal", "%s: too many attempts from %s", what, ip);
        kn_http_respond_error(req, 429, "too many requests");
        return 0;
    }
    const char *secret = g_mm.internal_secret;
    const char *bearer = kn_http_bearer(req);
    if (!secret[0] || !bearer || !kn_ct_streq(bearer, secret)) {
        KN_WARN("internal", "%s: rejected bad/missing auth from %s", what, ip);
        kn_http_set_header(req, "WWW-Authenticate", "Bearer realm=\"mm-internal\"");
        kn_http_respond_error(req, 401, "unauthorized");
        return 0;
    }
    /* The development default is public (it is in the repository): it only admits a direct
     * connection from this machine (no proxy in between: a proxied request carries X-Real-IP /
     * X-Forwarded-For, and with trust_proxy remote_addr is the header's address). */
    const int weak = !strcmp(secret, "kn-dev-secret") || strlen(secret) < MM_INTERNAL_MIN_SECRET;
    if (weak && (!mm_addr_is_loopback(ip) || kn_http_header_get(req, "X-Real-IP") || kn_http_header_get(req, "X-Forwarded-For"))) {
        KN_WARN("internal", "%s: refused %s: brain.internal_secret is the development default / short, local connections only", what, ip);
        kn_http_respond_error(req, 403, "forbidden");
        return 0;
    }
    return 1;
}
