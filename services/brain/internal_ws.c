/*
 * services/brain/internal_ws.c - /mm/internal/ws: game servers <-> brain
 * (v3.1.1 matchmaker/internalWs.ts + dbBroker.ts). Protocol: doc/backend.md
 * section 5. JSON text frames with a "type" discriminator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ai.h"
#include "brain_internal.h"
#include "game_presence.h"
#include "roles.h"
#include "strike_econ.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knsvc/ws.h"

#define MM_TICKET_RESUME_S (24 * 3600) /* a resumed ticket expired at most this long ago (auth "resume") */

#define HELLO_TIMEOUT_MS 10000

typedef struct NodeConn {
    Brain *b;
    kn_ws *ws;
    uint64_t id;
    int64_t opened_ms;
    int helloed;
    char node_id[MM_ID_MAX];
    char remote[64];
    struct NodeConn *next;
} NodeConn;

typedef struct Pending {
    uint64_t rid;
    uint64_t conn_id;
    kn_timer_id timer;
    InternalReplyCb cb;
    void *ud;
    struct Pending *next;
} Pending;

static NodeConn *g_conns;
static Pending *g_pending;
static uint64_t g_next_conn = 1;
static uint64_t g_next_rid = 1;
static kn_timer_id g_ping_timer;
static Brain *g_brain;
static int g_shutdown; /* brain going away: keep rows (TTL) so nodes reconnect seamlessly */

/* ---- helpers */

static NodeConn *conn_by_id(uint64_t id) {
    for (NodeConn *c = g_conns; c; c = c->next) if (c->id == id) return c;
    return NULL;
}

static NodeConn *conn_by_node(const char *node_id) {
    for (NodeConn *c = g_conns; c; c = c->next) {
        if (c->helloed && !strcmp(c->node_id, node_id)) return c;
    }
    return NULL;
}

static int send_frame(NodeConn *c, const cJSON *frame) {
    if (!c || !c->ws) return -1;
    char *text = kn_json_print(frame);
    if (!text) return -1;
    int rc = kn_ws_send_text(c->ws, text, (size_t) -1);
    free(text);
    return rc;
}

static void send_simple(NodeConn *c, const char *type, const char *key, const char *value) {
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", type);
    if (key) cJSON_AddStringToObject(f, key, value ? value : "");
    send_frame(c, f);
    cJSON_Delete(f);
}

int internal_ws_send(Brain *b, const char *node_id, const cJSON *frame) {
    (void) b;
    return send_frame(conn_by_node(node_id), frame);
}

/* Every connected (helloed) node of this brain gets the frame (not consumed). */
void internal_ws_broadcast(Brain *b, const cJSON *frame) {
    (void) b;
    for (NodeConn *c = g_conns; c; c = c->next) {
        if (c->helloed) send_frame(c, frame);
    }
}

int internal_ws_node_count(void) {
    int n = 0;
    for (NodeConn *c = g_conns; c; c = c->next) n += c->helloed;
    return n;
}

void internal_ws_kick(Brain *b, const char *node_id, int code, const char *reason) {
    (void) b;
    NodeConn *c = conn_by_node(node_id);
    if (!c) return;
    send_simple(c, "kick", "reason", reason);
    kn_ws_close(c->ws, code, reason);
}

/* ---- pending requests (brain -> node, matched by rid) */

static void pending_unlink(Pending *p) {
    for (Pending **pp = &g_pending; *pp; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            return;
        }
    }
}

static void pending_finish(Pending *p, const cJSON *reply, const char *err) {
    pending_unlink(p);
    if (p->timer) kn_loop_cancel_timer(g_brain->loop, p->timer);
    InternalReplyCb cb = p->cb;
    void *ud = p->ud;
    free(p);
    if (cb) cb(ud, reply, err);
}

static void pending_timeout(kn_loop *loop, void *ud) {
    (void) loop;
    Pending *p = ud;
    p->timer = 0;
    pending_finish(p, NULL, "timeout");
}

int internal_ws_request(Brain *b, const char *node_id, cJSON *frame, int timeout_ms,
                        InternalReplyCb cb, void *ud) {
    NodeConn *c = conn_by_node(node_id);
    if (!c) {
        cJSON_Delete(frame);
        return -1;
    }
    Pending *p = calloc(1, sizeof(*p));
    if (!p) {
        cJSON_Delete(frame);
        return -1;
    }
    p->rid = g_next_rid++;
    p->conn_id = c->id;
    p->cb = cb;
    p->ud = ud;
    p->next = g_pending;
    g_pending = p;
    cJSON_DeleteItemFromObjectCaseSensitive(frame, "rid");
    cJSON_AddNumberToObject(frame, "rid", (double) p->rid);
    int rc = send_frame(c, frame);
    cJSON_Delete(frame);
    if (rc) {
        pending_finish(p, NULL, "send failed");
        return 0;
    }
    p->timer = kn_loop_add_timer(b->loop, (uint64_t) (timeout_ms > 0 ? timeout_ms : 10000), 0, pending_timeout, p);
    return 0;
}

static int pending_resolve(NodeConn *c, const cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(frame, "rid");
    if (!cJSON_IsNumber(rid)) return 0;
    for (Pending *p = g_pending; p; p = p->next) {
        if (p->rid == (uint64_t) rid->valuedouble && p->conn_id == c->id) {
            pending_finish(p, frame, NULL);
            return 1;
        }
    }
    return 0;
}

static void pending_fail_conn(uint64_t conn_id) {
    for (Pending *p = g_pending; p;) {
        Pending *next = p->next;
        if (p->conn_id == conn_id) pending_finish(p, NULL, "disconnected");
        p = next;
    }
}

/* ---- replies that must survive the connection closing (async accounts calls) */

typedef struct ReplyCtx {
    uint64_t conn_id;
    double rid;
    char type[24];
} ReplyCtx;

static ReplyCtx *reply_ctx(NodeConn *c, const cJSON *frame, const char *type) {
    ReplyCtx *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->conn_id = c->id;
    r->rid = kn_json_num(frame, "rid", 0);
    snprintf(r->type, sizeof(r->type), "%s", type);
    return r;
}

/* Sends and frees reply_frame (adds type + rid). */
static void reply_send(ReplyCtx *r, cJSON *reply_frame) {
    NodeConn *c = conn_by_id(r->conn_id);
    if (c) {
        cJSON_AddStringToObject(reply_frame, "type", r->type);
        cJSON_AddNumberToObject(reply_frame, "rid", r->rid);
        send_frame(c, reply_frame);
    }
    cJSON_Delete(reply_frame);
}

/* acct -> acct-r */
static void on_acct_reply(void *ud, const char *err, const cJSON *data) {
    ReplyCtx *r = ud;
    cJSON *f = cJSON_CreateObject();
    if (err) cJSON_AddStringToObject(f, "err", err);
    else cJSON_AddNullToObject(f, "err");
    cJSON_AddItemToObject(f, "data", data ? cJSON_Duplicate(data, 1) : cJSON_CreateNull());
    reply_send(r, f);
    free(r);
}

/* auth -> auth-reply */
typedef struct AuthCtx {
    ReplyCtx r;
    cJSON *claims;
    Brain *b;
    int strike; /* a Krunker Strike game server ("flavor": "strike"): the account's Strike inventory */
} AuthCtx;

static void auth_finish(AuthCtx *a, const char *err, const cJSON *account) {
    cJSON *f = cJSON_CreateObject();
    cJSON_AddBoolToObject(f, "ok", err == NULL);
    if (err) cJSON_AddStringToObject(f, "err", err);
    cJSON_AddItemToObject(f, "account", account ? cJSON_Duplicate(account, 1) : cJSON_CreateNull());
    if (a->claims) cJSON_AddItemToObject(f, "ticket", cJSON_Duplicate(a->claims, 1));
    reply_send(&a->r, f);
    cJSON_Delete(a->claims);
    free(a);
}

static void on_auth_strike(void *ud, const char *err, const cJSON *data) {
    auth_finish(ud, err, err ? NULL : data);
}

static void on_auth_account(void *ud, const char *err, const cJSON *data) {
    AuthCtx *a = ud;
    /* a Strike server: Strike's items / krunkies / level, never Box Office's (strike_econ.c) */
    if (!err && data && a->strike) {
        strike_econ_overlay(a->b, data, on_auth_strike, a);
        return;
    }
    /* A valid ticket stays valid when the account lookup fails: report the
     * claims (guest-equivalent) but carry the error for the game server. */
    auth_finish(a, err, err ? NULL : data);
}

/* coord-auth: community role. A central account (auth service id + username) -> this
 * community's account row (inventory data[10], stats), created on first join and renamed
 * with the central account by coord-store's acct_ensure_central() (accounts.c). */
void acct_ensure_central(int64_t central_id, const char *username, AcctReplyFn cb, void *ud);

static void community_account(NodeConn *c, AuthCtx *a, int64_t central_id, const char *username) {
    if (central_id <= 0) {
        auth_finish(a, "Failed to verify username", NULL);
        return;
    }
    if (!brain_has_role(BRAIN_ROLE_COMMUNITY)) {
        auth_finish(a, "not a community coordinator", NULL);
        return;
    }
    (void) c;
    acct_ensure_central(central_id, username, on_auth_account, a);
}

typedef struct HjCtx {
    NodeConn *c;     /* may be gone by the reply: only conn_id (in a->r) is used to answer */
    uint64_t conn_id;
    AuthCtx *a;
} HjCtx;


static void on_has_joined(void *ud, const char *err, const cJSON *profile) {
    HjCtx *h = ud;
    NodeConn *c = conn_by_id(h->conn_id);
    if (err || !profile || !c) {
        auth_finish(h->a, err ? err : "Failed to verify username", NULL);
    } else {
        community_account(c, h->a, kn_json_int(profile, "id", 0), kn_json_str(profile, "username", ""));
    }
    free(h);
}

/* Krunker Strike: a Strike node's joins see the account's Strike inventory and its commits are
 * Strike progress (strike_econ.c), never Box Office stats. The node's hello flavor (registry) or the
 * frame's own "flavor" (server/src/accounts.c tags auth / commit frames) says so. */
static int node_is_strike(NodeConn *c, const cJSON *frame) {
    if (!strcmp(kn_json_str(frame, "flavor", ""), "strike")) return 1;
    MmNode *n = c->helloed ? registry_node(c->b->registry, c->node_id) : NULL;
    const char *f = n ? registry_node_flavor(n) : NULL;
    return f && !strcmp(f, "strike");
}

static void handle_auth(NodeConn *c, const cJSON *frame) {
    AuthCtx *a = calloc(1, sizeof(*a));
    if (!a) return;
    ReplyCtx *rc = reply_ctx(c, frame, "auth-reply");
    if (!rc) {
        free(a);
        return;
    }
    a->r = *rc;
    free(rc);
    a->b = c->b;
    a->strike = node_is_strike(c, frame);
    const char *ticket = kn_json_str(frame, "ticket", NULL);
    const char *token = kn_json_str(frame, "token", NULL);
    const cJSON *hj = cJSON_GetObjectItemCaseSensitive(frame, "hasJoined");
    const cJSON *central = cJSON_GetObjectItemCaseSensitive(frame, "central");
    if (cJSON_IsObject(hj)) { /* online_mode server: verify with the auth service, then the community row */
        HjCtx *h = calloc(1, sizeof(*h));
        if (!h) {
            auth_finish(a, "Server error.", NULL);
            return;
        }
        h->c = c;
        h->conn_id = c->id;
        h->a = a;
        auth_has_joined_async(c->b, kn_json_str(hj, "username", ""), kn_json_str(hj, "serverHash", ""),
                              kn_json_str(hj, "ip", ""), on_has_joined, h);
        return;
    }
    if (cJSON_IsObject(central)) { /* the game server verified it with hasJoined itself */
        community_account(c, a, kn_json_int(central, "id", 0), kn_json_str(central, "username", ""));
        return;
    }
    if (ticket && *ticket) {
        char err[128] = "";
        /* "resume": the node admitted this ticket before and a reconnect of that session sent it
         * again after its expiry (server/src/accounts.c acc_resume_allowed): signature only */
        const int resume = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(frame, "resume"));
        /* Security (doc/v7/command-auth.md): a resume skips the expiry only within
         * MM_TICKET_RESUME_S of it (it used to pass "now" = 1 s: any ticket ever issued) */
        const int64_t now_s = resume ? (int64_t) time(NULL) - MM_TICKET_RESUME_S : 0;
        a->claims = kn_signed_token_verify(mm_settings()->join_secret, ticket, now_s, err, sizeof(err));
        if (!a->claims) {
            auth_finish(a, err[0] ? err : "invalid ticket", NULL);
            return;
        }
        /* ... and a ticket only admits a player on the node it was issued for: a node holding the
         * shared secret can't turn another server's ticket into its account */
        if (strcmp(kn_json_str(a->claims, "nid", ""), c->node_id)) {
            KN_WARN("internal", "node \"%s\" sent a ticket for node \"%s\"", c->node_id, kn_json_str(a->claims, "nid", ""));
            auth_finish(a, "ticket is for another server", NULL);
            return;
        }
        /* the node counts the player from its READY (player-join) on: the seat stays until then,
         * shortened so a refused player frees it soon (registry.h MmReservation) */
        registry_ticket_authed(c->b->registry, kn_json_str(a->claims, "jti", ""), kn_wall_ms() + MM_AUTH_GRACE_MS);
        int64_t acc = kn_json_int(a->claims, "acc", 0);
        if (acc <= 0) {
            auth_finish(a, NULL, NULL);
            return;
        }
        cJSON *args = cJSON_CreateObject();
        cJSON_AddNumberToObject(args, "id", (double) acc);
        accounts_internal_call(c->b, "get", args, on_auth_account, a);
        cJSON_Delete(args);
        return;
    }
    if (token && *token) {
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "token", token);
        accounts_internal_call(c->b, "token", args, on_auth_account, a);
        cJSON_Delete(args);
        return;
    }
    auth_finish(a, "missing ticket/token", NULL);
}

/* commit -> one accounts "stats" call per account -> commit-reply */
typedef struct CommitCtx {
    ReplyCtx r;
    int outstanding;
    int all_ok;
    cJSON *results;
} CommitCtx;

typedef struct CommitItem {
    CommitCtx *ctx;
    int64_t account_id;
} CommitItem;

static void commit_done(CommitCtx *cc) {
    cJSON *f = cJSON_CreateObject();
    cJSON_AddBoolToObject(f, "ok", cc->all_ok);
    cJSON_AddItemToObject(f, "results", cc->results);
    reply_send(&cc->r, f);
    free(cc);
}

static void on_commit_stats(void *ud, const char *err, const cJSON *data) {
    CommitItem *it = ud;
    CommitCtx *cc = it->ctx;
    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "accountId", (double) it->account_id);
    cJSON_AddBoolToObject(res, "ok", err == NULL);
    if (err) {
        cc->all_ok = 0;
        cJSON_AddStringToObject(res, "err", err);
    } else if (data) {
        const char *keys[] = {"funds", "level", "score", "kr_awarded", "duplicate", "xp", "xpGained", "levelUp", "kr", "missions"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(data, keys[i]);
            if (v) cJSON_AddItemToObject(res, keys[i], cJSON_Duplicate(v, 1));
        }
    }
    cJSON_AddItemToArray(cc->results, res);
    free(it);
    if (--cc->outstanding == 0) commit_done(cc);
}

static void handle_commit(NodeConn *c, const cJSON *frame) {
    ReplyCtx *rc = reply_ctx(c, frame, "commit-reply");
    CommitCtx *cc = calloc(1, sizeof(*cc));
    if (!rc || !cc) {
        free(rc);
        free(cc);
        return;
    }
    cc->r = *rc;
    free(rc);
    cc->all_ok = 1;
    cc->results = cJSON_CreateArray();
    cc->outstanding = 1; /* guard so synchronous callbacks cannot finish early */
    MmNode *n = registry_node(c->b->registry, c->node_id);
    const char *lobby = kn_json_str(frame, "lobbyId", "");
    const char *match = kn_json_str(frame, "matchId", "");
    char match_key[160];
    snprintf(match_key, sizeof(match_key), "%s:%s", lobby, match);
    const cJSON *players = cJSON_GetObjectItemCaseSensitive(frame, "players");
    const int strike = node_is_strike(c, frame);
    const cJSON *p;
    cJSON_ArrayForEach(p, players) {
        int64_t acc = kn_json_int(p, "accountId", 0);
        if (acc <= 0) continue;
        cJSON *args = cJSON_Duplicate(p, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(args, "id");
        cJSON_AddNumberToObject(args, "id", (double) acc);
        if (!cJSON_GetObjectItemCaseSensitive(args, "match"))
            cJSON_AddStringToObject(args, "match", match_key);
        /* comp: v10 isComp, no end-screen 2x XP; classDouble: the round's class XP flag (double_xp.h) */
        const char *copy[] = {"mode", "map", "comp", "classDouble"};
        for (size_t i = 0; i < sizeof copy / sizeof *copy; i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(frame, copy[i]);
            if (v && !cJSON_GetObjectItemCaseSensitive(args, copy[i]))
                cJSON_AddItemToObject(args, copy[i], cJSON_Duplicate(v, 1));
        }
        cJSON_AddBoolToObject(args, "final", kn_json_bool(frame, "final", 1));
        if (n) {
            cJSON_AddStringToObject(args, "region", n->region);
            cJSON_AddStringToObject(args, "server", n->node_id);
        }
        CommitItem *it = calloc(1, sizeof(*it));
        if (!it) {
            cJSON_Delete(args);
            continue;
        }
        it->ctx = cc;
        it->account_id = acc;
        cc->outstanding++;
        if (strike) /* Krunker Strike: Strike's XP, krunkies, missions and stats (strike_econ.c) */
            strike_econ_commit(c->b, acc, args, kn_json_str(args, "match", match_key), on_commit_stats, it);
        else
            accounts_internal_call(c->b, "stats", args, on_commit_stats, it);
        cJSON_Delete(args);
    }
    if (--cc->outstanding == 0) commit_done(cc);
}

/* host-request -> brain_host_game -> host-request-reply */
static void on_host_request_done(void *ud, const char *err, const cJSON *reply) {
    ReplyCtx *r = ud;
    cJSON *f = reply ? cJSON_Duplicate(reply, 1) : cJSON_CreateObject();
    cJSON_AddBoolToObject(f, "ok", err == NULL);
    if (err) cJSON_AddStringToObject(f, "err", err);
    reply_send(r, f);
    free(r);
}

static void handle_host_request(NodeConn *c, const cJSON *frame) {
    ReplyCtx *r = reply_ctx(c, frame, "host-request-reply");
    if (!r) return;
    MmNode *n = registry_node(c->b->registry, c->node_id);
    const char *region = kn_json_str(frame, "region", n ? n->region : c->b->region);
    /* a node hosts for its own players: in its own flavor pool */
    brain_host_game(c->b, region, cJSON_GetObjectItemCaseSensitive(frame, "config"),
                    kn_json_int(frame, "hostAccountId", 0), kn_json_str(frame, "hostName", NULL), kn_json_str(frame, "hostGuestId", NULL),
                    n ? registry_node_flavor(n) : NULL, on_host_request_done, r);
}

/* ranked-result -> ranked.c -> ranked-result-reply */
static void on_ranked_result_done(void *ud, const char *err, const cJSON *reply) {
    ReplyCtx *r = ud;
    cJSON *f = reply ? cJSON_Duplicate(reply, 1) : cJSON_CreateObject();
    if (err) {
        cJSON_DeleteItemFromObjectCaseSensitive(f, "ok");
        cJSON_AddBoolToObject(f, "ok", 0);
        cJSON_AddStringToObject(f, "err", err);
    }
    reply_send(r, f);
    free(r);
}

/* bot-banter -> ai.c -> bot-banter-reply {rid, ok, text | err} (shared/src/bot_chat.c) */
static void on_bot_banter_done(void *ud, const char *err, const cJSON *reply) {
    ReplyCtx *r = ud;
    cJSON *f = reply ? cJSON_Duplicate(reply, 1) : cJSON_CreateObject();
    cJSON_AddBoolToObject(f, "ok", err == NULL);
    if (err) cJSON_AddStringToObject(f, "err", err);
    reply_send(r, f);
    free(r);
}

/* ---- frames */

static void handle_hello(NodeConn *c, const cJSON *frame) {
    Brain *b = c->b;
    if (c->helloed) {
        KN_WARN("internal", "duplicate hello from %s", c->node_id);
        return;
    }
    const char *node_id = kn_json_str(frame, "nodeId", "");
    if (!registry_valid_node_id(node_id)) {
        send_simple(c, "kick", "reason", "missing or invalid nodeId");
        kn_ws_close(c->ws, 4002, "missing nodeId");
        return;
    }
    NodeConn *old = conn_by_node(node_id);
    if (old && old != c) {
        /* newest wins (internalWs.ts: deploy restarts produce a clean reconnect) */
        old->helloed = 0;
        send_simple(old, "kick", "reason", "replaced");
        kn_ws_close(old->ws, 4003, "replaced");
    }
    MmNode *n = registry_node_hello(b->registry, node_id, frame, b->id, b->region, kn_wall_ms());
    if (!n) {
        kn_ws_close(c->ws, 1011, "registry");
        return;
    }
    snprintf(c->node_id, sizeof(c->node_id), "%s", node_id);
    c->helloed = 1;
    n->conn = (struct NodeConn *) c;
    n->takeover = 1;
    KN_INFO("internal", "node \"%s\" registered (region=%s, host=%s:%d, from %s, build %s protocol %s)", n->node_id,
            n->region, n->public_host, n->public_port, c->remote, kn_json_str(frame, "build", "?"),
            kn_json_str(frame, "protocol", "?"));
    const MmSettings *s = mm_settings();
    cJSON *w = cJSON_CreateObject();
    cJSON_AddStringToObject(w, "type", "welcome");
    cJSON_AddStringToObject(w, "assignedRegion", n->region);
    cJSON_AddStringToObject(w, "brainId", b->id);
    cJSON_AddStringToObject(w, "brainVersion", b->version);
    cJSON_AddNumberToObject(w, "now", (double) kn_wall_ms());
    cJSON_AddNumberToObject(w, "pingIntervalMs", s->ping_interval_s * 1000.0);
    cJSON_AddNumberToObject(w, "ttlMs", s->node_ttl_s * 1000.0);
    /* the live game events (Krunker v10.0.3 ongoingEvents: kill pools, community.c "gameEvents") */
    cJSON_AddItemToObject(w, "gameEvents", community_game_events(kn_wall_ms()));
    send_frame(c, w);
    cJSON_Delete(w);
    federation_node_changed(b, n);
}

static void handle_frame(NodeConn *c, const cJSON *frame) {
    Brain *b = c->b;
    const char *type = kn_json_str(frame, "type", "");
    if (!strcmp(type, "hello")) {
        handle_hello(c, frame);
        return;
    }
    if (!c->helloed) return;
    MmNode *n = registry_node(b->registry, c->node_id);
    if (!n) return;
    n->last_seen_ms = kn_wall_ms();

    if (!strcmp(type, "pong")) return;
    if (!strcmp(type, "lobby-up")) {
        if (registry_lobby_upsert(b->registry, n, cJSON_GetObjectItemCaseSensitive(frame, "lobby")) == 0)
            federation_node_changed(b, n);
        return;
    }
    if (!strcmp(type, "lobby-down")) {
        registry_lobby_remove(b->registry, n, kn_json_str(frame, "lobbyId", ""));
        federation_node_changed(b, n);
        return;
    }
    if (!strcmp(type, "snapshot")) {
        registry_lobbies_set(b->registry, n, cJSON_GetObjectItemCaseSensitive(frame, "lobbies"));
        federation_node_changed(b, n);
        return;
    }
    if (!strcmp(type, "drain")) {
        KN_INFO("internal", "node \"%s\" draining (%s)", n->node_id, kn_json_str(frame, "reason", ""));
        n->draining = 1;
        n->dirty = 1;
        federation_node_changed(b, n);
        return;
    }
    if (!strcmp(type, "player-join")) {
        /* The seat counts until the node's next report of the lobby (sent after this frame, so
         * it counts the player); federation deletes its mm_reservations row once that count is
         * persisted (federation.c persist_node). Releasing it here left the lobby looking empty
         * for the node's 250 ms lobby-up debounce: seekers in that window went to empty lobbies. */
        const char *jti = kn_json_str(frame, "clientId", kn_json_str(frame, "jti", NULL));
        if (jti && *jti) {
            char game_id[MM_GAME_ID_MAX] = "";
            const char *lobby = kn_json_str(frame, "lobbyId", "");
            if (federation_active() && strchr(lobby, ':')) snprintf(game_id, sizeof(game_id), "%s", lobby);
            else if (federation_active() && *lobby) snprintf(game_id, sizeof(game_id), "%s:%s", n->node_id, lobby);
            registry_arrive(b->registry, jti, game_id[0] ? game_id : NULL, kn_wall_ms(), MM_ARRIVE_GRACE_MS);
        }
        /* v10 presence:game: the account's friends see the game it plays in (game_presence.c) */
        game_presence_join(b, n->node_id, kn_json_str(frame, "lobbyId", ""), kn_json_int(frame, "accountId", 0));
        return;
    }
    if (!strcmp(type, "player-leave")) {
        game_presence_leave(b, n->node_id, kn_json_str(frame, "lobbyId", ""), kn_json_int(frame, "accountId", 0));
        return;
    }
    if (!strcmp(type, "acct") || !strcmp(type, "db-call")) {
        ReplyCtx *r = reply_ctx(c, frame, !strcmp(type, "acct") ? "acct-r" : "db-reply");
        if (!r) return;
        const cJSON *op = cJSON_GetObjectItemCaseSensitive(frame, "op");
        if (!op) op = cJSON_GetObjectItemCaseSensitive(frame, "opcode");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(frame, "args");
        accounts_internal_call(b, cJSON_IsString(op) ? op->valuestring : "", args, on_acct_reply, r);
        return;
    }
    if (!strcmp(type, "auth")) {
        handle_auth(c, frame);
        return;
    }
    if (!strcmp(type, "commit")) {
        handle_commit(c, frame);
        return;
    }
    if (!strcmp(type, "ranked-result")) {
        ReplyCtx *r = reply_ctx(c, frame, "ranked-result-reply");
        if (r) ranked_result(b, frame, on_ranked_result_done, r);
        return;
    }
    if (!strcmp(type, "ranked-cancel")) { /* a ranked lobby that never filled (v10 endGame(3), no rating) */
        ranked_cancel(b, frame);
        return;
    }
    if (!strcmp(type, "turf-kills")) { /* v10 Turf Wars: onRecordTerritorialWarsKills (turf.c) */
        ReplyCtx *r = reply_ctx(c, frame, "turf-kills-reply");
        if (r) turf_record(b, frame, on_ranked_result_done, r);
        return;
    }
    if (!strcmp(type, "host-request")) {
        handle_host_request(c, frame);
        return;
    }
    if (!strcmp(type, "bot-banter")) { /* bot chat: one AI-written line for a bot player (ai.c) */
        ReplyCtx *r = reply_ctx(c, frame, "bot-banter-reply");
        if (r) ai_bot_banter(b, frame, on_bot_banter_done, r);
        return;
    }
    if (!strcmp(type, "host-reply") || !strcmp(type, "reply") || !strcmp(type, "rcon-reply")) {
        pending_resolve(c, frame);
        return;
    }
    /* unknown frames are ignored (forward compatible) */
}

/* ---- WebSocket callbacks */

static void ws_on_message(kn_ws *ws, const void *data, size_t len, int is_binary, void *ud) {
    (void) ws;
    NodeConn *c = ud;
    cJSON *frame = kn_json_parse(data, len);
    if (!cJSON_IsObject(frame)) {
        KN_WARN("internal", "bad frame from %s (%s)", c->node_id[0] ? c->node_id : c->remote,
                is_binary ? "binary" : "text");
        cJSON_Delete(frame);
        return;
    }
    handle_frame(c, frame);
    cJSON_Delete(frame);
}

static void ws_on_close(kn_ws *ws, int code, const char *reason, void *ud) {
    (void) ws;
    NodeConn *c = ud;
    Brain *b = c->b;
    for (NodeConn **pp = &g_conns; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    pending_fail_conn(c->id);
    if (c->helloed) {
        MmNode *n = registry_node(b->registry, c->node_id);
        if (n && n->conn == (struct NodeConn *) c) {
            KN_INFO("internal", "node \"%s\" disconnected (%d %s)", c->node_id, code, reason ? reason : "");
            registry_node_remove(b->registry, c->node_id);
            game_presence_node_gone(b, c->node_id); /* its players are in no game any more */
            if (!g_shutdown) federation_node_gone(b, c->node_id);
        }
    }
    free(c);
}

static void internal_ws_handler(kn_http_req *req, void *ud) {
    Brain *b = ud;
    if (!kn_http_is_ws_upgrade(req)) {
        kn_http_respond_error(req, 426, "upgrade required");
        return;
    }
    if (!mm_internal_authorized(req, "internal ws upgrade")) return; /* rate limit, secret, dev default (brain_util.c) */
    if (b->draining) {
        kn_http_respond_error(req, 503, "updating");
        return;
    }
    NodeConn *c = calloc(1, sizeof(*c));
    if (!c) {
        kn_http_respond_error(req, 500, "internal");
        return;
    }
    c->b = b;
    c->id = g_next_conn++;
    c->opened_ms = kn_wall_ms();
    snprintf(c->remote, sizeof(c->remote), "%s", req->remote_addr ? req->remote_addr : "?");
    static const kn_ws_callbacks cbs = {.on_message = ws_on_message, .on_close = ws_on_close};
    kn_ws_options opts = {.max_message_bytes = 4u << 20};
    c->ws = kn_ws_accept(req, &cbs, &opts, c);
    if (!c->ws) {
        free(c);
        return;
    }
    c->next = g_conns;
    g_conns = c;
    KN_INFO("internal", "connection from %s", c->remote);
}

/* The game events running now changed (one started or ended, the feed was edited): "game-events"
 * {events} to every node, which replaces its ongoingEvents (server/src/accounts.c). Checked each ping. */
static char *g_game_events_sent;

static void game_events_push(void) {
    cJSON *events = community_game_events(kn_wall_ms());
    char *text = cJSON_PrintUnformatted(events);
    if (!text || (g_game_events_sent && !strcmp(text, g_game_events_sent))) {
        free(text);
        cJSON_Delete(events);
        return;
    }
    free(g_game_events_sent);
    g_game_events_sent = text;
    const int running = cJSON_GetArraySize(events);
    cJSON *f = cJSON_CreateObject(); /* (a node the welcome just told gets the same set again: harmless) */
    cJSON_AddStringToObject(f, "type", "game-events");
    cJSON_AddItemToObject(f, "events", events);
    for (NodeConn *c = g_conns; c; c = c->next)
        if (c->helloed) send_frame(c, f);
    cJSON_Delete(f);
    KN_INFO("internal", "game events: %d running", running);
}

/* Ping every node; close links that sent nothing for node_ttl_s (registry.ts reapStale). */
static void ping_tick(kn_loop *loop, void *ud) {
    (void) loop;
    Brain *b = ud;
    const MmSettings *s = mm_settings();
    int64_t now = kn_wall_ms();
    cJSON *ping = cJSON_CreateObject();
    cJSON_AddStringToObject(ping, "type", "ping");
    cJSON_AddNumberToObject(ping, "ts", (double) now);
    uint64_t stale[64];
    size_t nstale = 0;
    for (NodeConn *c = g_conns; c; c = c->next) {
        if (!c->helloed) {
            if (now - c->opened_ms > HELLO_TIMEOUT_MS) kn_ws_close(c->ws, 4002, "no hello");
            continue;
        }
        MmNode *n = registry_node(b->registry, c->node_id);
        if (!n) continue;
        if (now - n->last_seen_ms > (int64_t) s->node_ttl_s * 1000) {
            if (nstale < sizeof(stale) / sizeof(*stale)) stale[nstale++] = c->id;
            continue;
        }
        send_frame(c, ping);
    }
    cJSON_Delete(ping);
    /* a hung peer would not answer a close handshake: drop the TCP link now
     * (on_close runs synchronously and unlinks the conn, hence the id list) */
    for (size_t i = 0; i < nstale; i++) {
        NodeConn *c = conn_by_id(stale[i]);
        if (!c) continue;
        KN_WARN("internal", "reaping stale node \"%s\" (no frame for %ds)", c->node_id, s->node_ttl_s);
        kn_ws_abort(c->ws);
    }
    registry_expire(b->registry, now);
    game_events_push();
}

int internal_ws_init(Brain *b) {
    g_brain = b;
    kn_http_route(b->http, "GET", "/mm/internal/ws", internal_ws_handler, b);
    kn_http_route(b->http, "GET", "/internal/ws", internal_ws_handler, b);
    uint64_t every = (uint64_t) mm_settings()->ping_interval_s * 1000;
    /* the reaper must run more often than the TTL */
    uint64_t ttl_half = (uint64_t) mm_settings()->node_ttl_s * 500;
    if (ttl_half && ttl_half < every) every = ttl_half;
    g_ping_timer = kn_loop_add_timer(b->loop, every, every, ping_tick, b);
    return 0;
}

void internal_ws_shutdown(Brain *b) {
    if (g_ping_timer) kn_loop_cancel_timer(b->loop, g_ping_timer);
    g_ping_timer = 0;
    g_shutdown = 1;
    free(g_game_events_sent);
    g_game_events_sent = NULL;
    /* going away: nodes keep their lobbies and reconnect (backoff) to the
     * restarted brain; rows expire by TTL if nobody takes them over. */
    for (NodeConn *c = g_conns; c; c = c->next) kn_ws_close(c->ws, KN_WS_CLOSE_GOING_AWAY, "brain shutting down");
}
