/*
 * services/brain/federation.c - leader-free multi-brain registry over the
 * shared database (doc/backend.md section 6, migration <dialect>/0200_matchmaker.sql;
 * multi-statement writes are kn_procs in db_mm.c). On drivers without native
 * pub/sub (sqlite, mysql) the NOTIFYs below are kn_pubsub rows polled every
 * database.notify_poll_ms (knsvc/db.h), and the 1 s refresh covers lost ones.
 *
 * - Each brain persists the nodes connected to it (mm_nodes + mm_lobbies) on
 *   every change and bumps their heartbeat every brain.heartbeat_s.
 * - Every second (and on NOTIFY kn_mm_lobby) it reloads the rows of the other
 *   brains' live nodes into the registry as REMOTE nodes.
 * - Reservations (join tickets) go to mm_reservations so all brains count them.
 * - Host requests for another brain's node go through mm_host_requests +
 *   NOTIFY kn_mm_host / kn_mm_host_done; `assign` heads-ups through
 *   NOTIFY kn_mm_assign.
 * Without a DB (or brain.federation=false) everything here is a no-op and the
 * brain only knows its own nodes (v3.1.1 behaviour).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_internal.h"
#include "knsvc/db.h"
#include "db_procs.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define REFRESH_MS 1000
#define FLUSH_DELAY_MS 100
#define HOST_POLL_MS 500

static Brain *g_b;
static int g_active;
static kn_timer_id g_hb_timer, g_refresh_timer, g_flush_timer, g_host_poll_timer;
static int g_refresh_inflight;
static int g_refresh_again;

int federation_active(void) {
    return g_active;
}

static char g_ttl_s[16];

static void log_db_error(const char *what, kn_db_result *res) {
    KN_WARN("federation", "%s failed: %s", what, kn_db_error(res));
}

static void on_ignore(kn_db_result *res, void *ud) {
    if (!kn_db_ok(res)) log_db_error(ud ? (const char *) ud : "query", res);
}

/* ---------------------------------------------------------------- persist */

static void schedule_flush(void);

typedef struct PersistCtx {
    char node_id[MM_ID_MAX];
} PersistCtx;

/* node persist statements: P_MM_NODE_TAKEOVER / P_MM_NODE_UPDATE (db_mm.c, the
 * lobby tuples keep their key order and connected_at is the hello time) */
static const kn_sql SQL_NODE_DELETE = {.pg = "DELETE FROM mm_nodes WHERE node_id = $1::text AND brain_id = $2::text",
                                       .any = "DELETE FROM mm_nodes WHERE node_id = $1 AND brain_id = $2"};

static char *lobbies_json(const MmNode *n) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n->lobby_count; i++) {
        const MmLobby *l = &n->lobbies[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "gid", l->game_id);
        cJSON_AddStringToObject(o, "lid", l->local_id);
        const cJSON *reg = cJSON_GetArrayItem(l->tuple, 1);
        cJSON_AddStringToObject(o, "region", cJSON_IsString(reg) ? reg->valuestring : n->region);
        cJSON_AddNumberToObject(o, "players", l->players);
        cJSON_AddNumberToObject(o, "max", l->max_players);
        cJSON_AddBoolToObject(o, "cs", l->custom);
        cJSON_AddBoolToObject(o, "pr", l->protect);
        cJSON_AddStringToObject(o, "mode", l->mode);
        cJSON_AddStringToObject(o, "map", l->map);
        cJSON_AddStringToObject(o, "v", l->version);
        cJSON_AddItemToObject(o, "tuple", cJSON_Duplicate(l->tuple, 1));
        cJSON_AddItemToArray(arr, o);
    }
    char *s = kn_json_print(arr);
    cJSON_Delete(arr);
    return s;
}

static void on_persist(kn_db_result *res, void *ud) {
    PersistCtx *p = ud;
    MmNode *n = registry_node(g_b->registry, p->node_id);
    if (!kn_db_ok(res)) {
        log_db_error("node persist", res);
        if (n) n->dirty = 1; /* retry on the next flush/heartbeat */
    } else if (n && n->local && kn_db_get_int64(res, 0, 0, 0) == 0) {
        KN_WARN("federation", "node \"%s\" is now owned by another brain; dropping our link", p->node_id);
        internal_ws_kick(g_b, p->node_id, 4003, "replaced");
    }
    if (n) {
        n->persisting = 0;
        if (n->dirty) schedule_flush();
    } else if (kn_db_ok(res)) {
        /* removed while the write was in flight: make sure it stays gone */
        kn_db_query_sql(g_b->db, &SQL_NODE_DELETE, KN_DB_ARGS(p->node_id, g_b->id), on_ignore, "node delete");
    }
    free(p);
    kn_db_notify(g_b->db, "kn_mm_lobby", g_b->id);
}

static void persist_node(MmNode *n) {
    PersistCtx *p = calloc(1, sizeof(*p));
    char *lobbies = lobbies_json(n);
    if (!p || !lobbies) {
        free(p);
        free(lobbies);
        return;
    }
    snprintf(p->node_id, sizeof(p->node_id), "%s", n->node_id);
    char port[16], maxp[16], maxl[16];
    snprintf(port, sizeof(port), "%d", n->public_port);
    snprintf(maxp, sizeof(maxp), "%d", n->max_players);
    snprintf(maxl, sizeof(maxl), "%d", n->max_lobbies);
    char connected[32];
    snprintf(connected, sizeof(connected), "%lld", (long long) n->connected_ms);
    const int takeover = n->takeover;
    n->takeover = 0;
    n->dirty = 0;
    n->persisting = 1;
    if (takeover)
        kn_db_call(g_b->db, &P_MM_NODE_TAKEOVER,
                    KN_DB_ARGS(n->node_id, g_b->id, n->region, n->name, n->public_host, port, n->web_url,
                               n->version, maxp, maxl, n->custom ? "true" : "false", n->draining ? "true" : "false",
                               lobbies, connected),
                    on_persist, p);
    else
        kn_db_call(g_b->db, &P_MM_NODE_UPDATE,
                    KN_DB_ARGS(n->node_id, g_b->id, n->region, n->name, n->public_host, port, n->web_url,
                               n->version, maxp, maxl, n->custom ? "true" : "false", n->draining ? "true" : "false",
                               lobbies),
                    on_persist, p);
    free(lobbies);
}

static void flush_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    g_flush_timer = 0;
    Registry *r = g_b->registry;
    for (size_t i = 0; i < r->node_count; i++) {
        MmNode *n = r->nodes[i];
        if (n->local && n->dirty && !n->persisting) persist_node(n);
    }
}

static void schedule_flush(void) {
    if (!g_flush_timer) g_flush_timer = kn_loop_add_timer(g_b->loop, FLUSH_DELAY_MS, 0, flush_tick, NULL);
}

void federation_node_changed(Brain *b, MmNode *n) {
    (void) b;
    n->dirty = 1;
    if (g_active) schedule_flush();
}

void federation_node_gone(Brain *b, const char *node_id) {
    if (!g_active) return;
    kn_db_query_sql(b->db, &SQL_NODE_DELETE, KN_DB_ARGS(node_id, b->id), on_ignore, "node delete");
    kn_db_notify(b->db, "kn_mm_lobby", b->id);
}

void federation_reserve(Brain *b, const char *jti, const char *game_id, int64_t account_id, int ttl_s) {
    if (!g_active) return;
    char acc[32], ttl[16];
    snprintf(acc, sizeof(acc), "%lld", (long long) account_id);
    snprintf(ttl, sizeof(ttl), "%d", ttl_s);
    static const kn_sql q = {
        .pg = "INSERT INTO mm_reservations (jti, game_id, brain_id, account_id, expires_at)"
              " VALUES ($1::text, $2::text, $3::text, NULLIF($4::bigint, 0), now() + make_interval(secs => $5::int))"
              " ON CONFLICT (jti) DO NOTHING",
        .any = "{insert_ignore} INTO mm_reservations (jti, game_id, brain_id, account_id, expires_at)"
               " VALUES ($1, $2, $3, NULLIF({int:$4}, 0), {later:$5}) {ignore_conflicts}"};
    kn_db_query_sql(b->db, &q, KN_DB_ARGS(jti, game_id, b->id, acc, ttl), on_ignore, "reserve");
}

void federation_release(Brain *b, const char *jti) {
    if (!g_active) return;
    static const kn_sql q = {.pg = "DELETE FROM mm_reservations WHERE jti = $1::text",
                             .any = "DELETE FROM mm_reservations WHERE jti = $1"};
    kn_db_query_sql(b->db, &q, KN_DB_ARGS(jti), on_ignore, "release");
}

/* ---------------------------------------------------------------- heartbeat + reaper */

static void heartbeat_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    Brain *b = g_b;
    char started[32];
    snprintf(started, sizeof(started), "%lld", (long long) b->started_ms);
    static const kn_sql q = {
        .pg = "INSERT INTO mm_brains (brain_id, region, public_url, version, started_at, heartbeat_at)"
              " VALUES ($1::text, $2::text, $3::text, $4::text, to_timestamp($5::double precision / 1000), now())"
              " ON CONFLICT (brain_id) DO UPDATE SET region = EXCLUDED.region, public_url = EXCLUDED.public_url,"
              "   version = EXCLUDED.version, started_at = EXCLUDED.started_at, heartbeat_at = now()",
        .sqlite = "INSERT INTO mm_brains (brain_id, region, public_url, version, started_at, heartbeat_at)"
                  " VALUES ($1, $2, $3, $4, {from_ms:$5}, {now})"
                  " ON CONFLICT (brain_id) DO UPDATE SET region = excluded.region, public_url = excluded.public_url,"
                  "   version = excluded.version, started_at = excluded.started_at, heartbeat_at = excluded.heartbeat_at",
        .mysql = "INSERT INTO mm_brains (brain_id, region, public_url, version, started_at, heartbeat_at)"
                 " VALUES ($1, $2, $3, $4, {from_ms:$5}, {now})"
                 " ON DUPLICATE KEY UPDATE region = VALUES(region), public_url = VALUES(public_url),"
                 "   version = VALUES(version), started_at = VALUES(started_at), heartbeat_at = VALUES(heartbeat_at)"};
    kn_db_query_sql(b->db, &q,
                KN_DB_ARGS(b->id, b->region, mm_settings()->public_url, b->version, started),
                on_ignore, "brain heartbeat");
    /* local nodes: one UPDATE for all; ids are [A-Za-z0-9_.-] so a plain array literal is safe */
    kn_buf ids = {0};
    kn_buf_append_byte(&ids, '{');
    int count = 0;
    Registry *r = b->registry;
    for (size_t i = 0; i < r->node_count; i++) {
        if (!r->nodes[i]->local) continue;
        if (count++) kn_buf_append_byte(&ids, ',');
        kn_buf_append_str(&ids, r->nodes[i]->node_id);
        if (r->nodes[i]->dirty && !r->nodes[i]->persisting) schedule_flush();
    }
    kn_buf_append_byte(&ids, '}');
    if (count) {
        kn_db_call(b->db, &P_MM_NODE_HEARTBEAT, KN_DB_ARGS(b->id, ids.data), on_ignore, "node heartbeat");
    }
    kn_buf_free(&ids);
    /* reaper (any brain; idempotent) */
    static const kn_sql reap_nodes = {.pg = "DELETE FROM mm_nodes WHERE heartbeat_at < now() - make_interval(secs => 3 * $1::int)",
                                      .any = "DELETE FROM mm_nodes WHERE heartbeat_at < {ago:3 * {int:$1}}"},
                        reap_res = {.pg = "DELETE FROM mm_reservations WHERE expires_at < now()",
                                    .any = "DELETE FROM mm_reservations WHERE expires_at < {now}"},
                        reap_host = {.pg = "DELETE FROM mm_host_requests WHERE created_at < now() - interval '10 minutes'",
                                     .any = "DELETE FROM mm_host_requests WHERE created_at < {ago:600}"},
                        reap_brains = {.pg = "DELETE FROM mm_brains WHERE heartbeat_at < now() - interval '1 day'",
                                       .any = "DELETE FROM mm_brains WHERE heartbeat_at < {ago:86400}"};
    kn_db_query_sql(b->db, &reap_nodes, KN_DB_ARGS(g_ttl_s), on_ignore, "reap nodes");
    kn_db_query_sql(b->db, &reap_res, 0, NULL, on_ignore, "reap reservations");
    kn_db_query_sql(b->db, &reap_host, 0, NULL, on_ignore, "reap host requests");
    kn_db_query_sql(b->db, &reap_brains, 0, NULL, on_ignore, "reap brains");
}

/* ---------------------------------------------------------------- refresh (remote view) */

typedef struct RefreshCtx {
    int step;
} RefreshCtx;

static void refresh_start(void);

static void apply_remote_nodes(kn_db_result *res) {
    Registry *r = g_b->registry;
    registry_remote_begin(r);
    int rows = kn_db_rows(res);
    for (int i = 0; i < rows; i++) {
        const char *node_id = kn_db_get_by(res, i, "node_id");
        MmNode *n = registry_remote_node(r, node_id);
        if (!n) continue;
        snprintf(n->brain_id, sizeof(n->brain_id), "%s", kn_db_get_by(res, i, "brain_id"));
        snprintf(n->region, sizeof(n->region), "%s", kn_db_get_by(res, i, "region"));
        snprintf(n->name, sizeof(n->name), "%s", kn_db_get_by(res, i, "name"));
        snprintf(n->public_host, sizeof(n->public_host), "%s", kn_db_get_by(res, i, "public_host"));
        snprintf(n->web_url, sizeof(n->web_url), "%s", kn_db_get_by(res, i, "web_url"));
        snprintf(n->version, sizeof(n->version), "%s", kn_db_get_by(res, i, "version"));
        n->public_port = (int) kn_db_get_int64(res, i, kn_db_col(res, "public_port"), 0);
        n->max_players = (int) kn_db_get_int64(res, i, kn_db_col(res, "max_players"), 8);
        n->max_lobbies = (int) kn_db_get_int64(res, i, kn_db_col(res, "max_lobbies"), 1);
        n->custom = kn_db_get_bool(res, i, kn_db_col(res, "custom"), 0);
        n->draining = kn_db_get_bool(res, i, kn_db_col(res, "draining"), 0);
        n->last_seen_ms = kn_db_get_int64(res, i, kn_db_col(res, "hb"), 0);
        n->local = 0;
        n->conn = NULL;
        const char *lob = kn_db_get_by(res, i, "lobbies");
        cJSON *arr = lob ? kn_json_parse(lob, strlen(lob)) : NULL;
        const cJSON *t;
        cJSON_ArrayForEach(t, arr) registry_lobby_upsert(r, n, t);
        cJSON_Delete(arr);
        n->dirty = 0;
    }
    registry_remote_end(r);
}

static void apply_reservations(kn_db_result *res) {
    Registry *r = g_b->registry;
    for (size_t i = 0; i < r->node_count; i++) {
        for (size_t j = 0; j < r->nodes[i]->lobby_count; j++) r->nodes[i]->lobbies[j].reserved_remote = 0;
    }
    int rows = kn_db_rows(res);
    for (int i = 0; i < rows; i++) {
        MmLobby *l = registry_lobby(r, kn_db_get(res, i, 0), NULL);
        if (l) l->reserved_remote = (int) kn_db_get_int64(res, i, 1, 0);
    }
}

static void apply_brains(kn_db_result *res) {
    Registry *r = g_b->registry;
    int rows = kn_db_rows(res);
    MmBrainInfo *list = rows > 0 ? calloc((size_t) rows, sizeof(*list)) : NULL;
    if (rows > 0 && !list) return;
    for (int i = 0; i < rows; i++) {
        snprintf(list[i].brain_id, sizeof(list[i].brain_id), "%s", kn_db_get(res, i, 0));
        snprintf(list[i].region, sizeof(list[i].region), "%s", kn_db_get(res, i, 1));
        snprintf(list[i].public_url, sizeof(list[i].public_url), "%s", kn_db_get(res, i, 2));
        list[i].heartbeat_ms = kn_db_get_int64(res, i, 3, 0);
    }
    free(r->brains);
    r->brains = list;
    r->brain_count = (size_t) (rows > 0 ? rows : 0);
}

/* step 0 (live nodes of the other brains + their lobby tuples) is P_MM_REFRESH_NODES */
static const kn_sql REFRESH_SQL[3] = {
    {.pg = NULL},
    /* every query of the refresh takes the same two parameters ($1 brain id,
     * $2 ttl seconds) and must USE both with an explicit type: an unused
     * parameter makes PostgreSQL fail with "could not determine data type of
     * parameter $n" (caught by tests/federation_test.sh's log check) */
    {.pg = "SELECT game_id, count(*) FROM mm_reservations"
           " WHERE brain_id <> $1::text AND expires_at > now() - make_interval(secs => 0 * $2::int) GROUP BY game_id",
     .any = "SELECT game_id, count(*) FROM mm_reservations WHERE brain_id <> $1 AND expires_at > {now} GROUP BY game_id"},
    {.pg = "SELECT brain_id, region, public_url, (extract(epoch FROM heartbeat_at) * 1000)::bigint FROM mm_brains"
           " WHERE heartbeat_at > now() - make_interval(secs => $2::int) OR brain_id = $1::text ORDER BY brain_id",
     .any = "SELECT brain_id, region, public_url, {epoch_ms:heartbeat_at} FROM mm_brains"
            " WHERE heartbeat_at > {ago:$2} OR brain_id = $1 ORDER BY brain_id"}};

static void refresh_step(int step);

/* Steps run one after another (nodes, then reservations on the new lobbies, then brains). */
static void on_refresh(kn_db_result *res, void *ud) {
    RefreshCtx *c = ud;
    int step = c->step;
    free(c);
    if (!g_active) return;
    if (!kn_db_ok(res)) {
        log_db_error("refresh", res);
    } else if (step == 0) {
        apply_remote_nodes(res);
    } else if (step == 1) {
        apply_reservations(res);
    } else {
        apply_brains(res);
    }
    if (step < 2) {
        refresh_step(step + 1);
        return;
    }
    g_refresh_inflight = 0;
    if (g_refresh_again) {
        g_refresh_again = 0;
        refresh_start();
    }
}

static void refresh_step(int step) {
    RefreshCtx *c = calloc(1, sizeof(*c));
    if (!c) {
        g_refresh_inflight = 0;
        return;
    }
    c->step = step;
    if (step == 0) kn_db_call(g_b->db, &P_MM_REFRESH_NODES, KN_DB_ARGS(g_b->id, g_ttl_s), on_refresh, c);
    else kn_db_query_sql(g_b->db, &REFRESH_SQL[step], KN_DB_ARGS(g_b->id, g_ttl_s), on_refresh, c);
}

static void refresh_start(void) {
    if (!g_active) return;
    if (g_refresh_inflight) {
        g_refresh_again = 1;
        return;
    }
    g_refresh_inflight = 1;
    refresh_step(0);
}

static void refresh_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    refresh_start();
}

/* ---------------------------------------------------------------- assign relay */

void federation_assign_remote(Brain *b, const MmNode *n, const cJSON *assign) {
    if (!g_active || !n->brain_id[0]) return;
    cJSON *env = cJSON_CreateObject();
    cJSON_AddStringToObject(env, "brain", n->brain_id);
    cJSON_AddStringToObject(env, "node", n->node_id);
    cJSON_AddItemToObject(env, "frame", cJSON_Duplicate(assign, 1));
    char *s = kn_json_print(env);
    cJSON_Delete(env);
    if (s && strlen(s) < 7900) kn_db_notify(b->db, "kn_mm_assign", s);
    free(s);
}

static void on_notify_assign(const char *channel, const char *payload, void *ud) {
    (void) channel;
    (void) ud;
    cJSON *env = payload ? kn_json_parse(payload, strlen(payload)) : NULL;
    if (env && !strcmp(kn_json_str(env, "brain", ""), g_b->id)) {
        internal_ws_send(g_b, kn_json_str(env, "node", ""), cJSON_GetObjectItemCaseSensitive(env, "frame"));
    }
    cJSON_Delete(env);
}

/* ---------------------------------------------------------------- remote host requests */

typedef struct HostWait {
    int64_t id;
    InternalReplyCb cb;
    void *ud;
    int64_t deadline_ms;
    int checking;
    struct HostWait *next;
} HostWait;

static HostWait *g_waits;

static void wait_finish(HostWait *w, const cJSON *reply, const char *err) {
    for (HostWait **pp = &g_waits; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    }
    InternalReplyCb cb = w->cb;
    void *ud = w->ud;
    free(w);
    if (cb) cb(ud, reply, err);
}

typedef struct HostInsert {
    InternalReplyCb cb;
    void *ud;
    char target_brain[64];
} HostInsert;

static void on_host_inserted(kn_db_result *res, void *ud) {
    HostInsert *hi = ud;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        log_db_error("host request insert", res);
        InternalReplyCb cb = hi->cb;
        void *cud = hi->ud;
        free(hi);
        if (cb) cb(cud, NULL, "db error");
        return;
    }
    HostWait *w = calloc(1, sizeof(*w));
    if (!w) {
        InternalReplyCb cb = hi->cb;
        void *cud = hi->ud;
        free(hi);
        if (cb) cb(cud, NULL, "oom");
        return;
    }
    w->id = kn_db_get_int64(res, 0, 0, 0);
    w->cb = hi->cb;
    w->ud = hi->ud;
    w->deadline_ms = kn_wall_ms() + (int64_t) mm_settings()->host_timeout_s * 1000;
    w->next = g_waits;
    g_waits = w;
    char payload[128];
    snprintf(payload, sizeof(payload), "%lld:%s", (long long) w->id, hi->target_brain);
    kn_db_notify(g_b->db, "kn_mm_host", payload);
    free(hi);
}

int federation_host_remote(Brain *b, const MmNode *n, const char *region, const cJSON *config,
                           int64_t account_id, const char *account_name, InternalReplyCb cb, void *ud) {
    if (!g_active) return -1;
    HostInsert *hi = calloc(1, sizeof(*hi));
    if (!hi) return -1;
    hi->cb = cb;
    hi->ud = ud;
    snprintf(hi->target_brain, sizeof(hi->target_brain), "%s", n->brain_id);
    char *cfg = config ? kn_json_print(config) : NULL;
    char acc[32];
    snprintf(acc, sizeof(acc), "%lld", (long long) account_id);
    kn_db_call(b->db, &P_MM_HOST_REQUEST,
                KN_DB_ARGS(b->id, n->brain_id, n->node_id, region ? region : "", cfg, acc,
                           account_name ? account_name : ""),
                on_host_inserted, hi);
    free(cfg);
    return 0;
}

/* requester side: check a request's row */
static void on_wait_row(kn_db_result *res, void *ud) {
    int64_t id = (int64_t) (intptr_t) ud;
    HostWait *w = NULL;
    for (HostWait *it = g_waits; it; it = it->next) if (it->id == id) w = it;
    if (!w) return;
    w->checking = 0;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) return;
    const char *status = kn_db_get(res, 0, 0);
    if (strcmp(status, "done") && strcmp(status, "failed")) return;
    const char *result = kn_db_get(res, 0, 1);
    cJSON *reply = kn_json_parse(result, strlen(result));
    if (!strcmp(status, "done") && reply) wait_finish(w, reply, NULL);
    else wait_finish(w, NULL, reply ? kn_json_str(reply, "err", "host failed") : "host failed");
    cJSON_Delete(reply);
}

static void check_wait(HostWait *w) {
    if (w->checking) return;
    w->checking = 1;
    char id[32];
    snprintf(id, sizeof(id), "%lld", (long long) w->id);
    static const kn_sql q = {.pg = "SELECT status, coalesce(result::text, 'null') FROM mm_host_requests WHERE id = $1::bigint",
                             .any = "SELECT status, coalesce(result, 'null') AS result__t FROM mm_host_requests WHERE id = $1"};
    kn_db_query_sql(g_b->db, &q, KN_DB_ARGS(id), on_wait_row, (void *) (intptr_t) w->id);
}

/* target side: forward a claimed request to our node */
typedef struct HostServe {
    int64_t id;
    char from_brain[64];
} HostServe;

static void host_serve_store(HostServe *hs, const cJSON *result, int ok) {
    char id[32];
    snprintf(id, sizeof(id), "%lld", (long long) hs->id);
    char *res = kn_json_print(result);
    static const kn_sql q = {
        .pg = "UPDATE mm_host_requests SET status = $2::text, result = $3::jsonb, updated_at = now() WHERE id = $1::bigint",
        .any = "UPDATE mm_host_requests SET status = $2, result = $3, updated_at = {now} WHERE id = $1"};
    kn_db_query_sql(g_b->db, &q,
                KN_DB_ARGS(id, ok ? "done" : "failed", res ? res : "null"), on_ignore, "host request result");
    free(res);
    char payload[128];
    snprintf(payload, sizeof(payload), "%s:%s", id, hs->from_brain);
    kn_db_notify(g_b->db, "kn_mm_host_done", payload);
    free(hs);
}

static void on_served_host_reply(void *ud, const cJSON *reply, const char *err) {
    HostServe *hs = ud;
    cJSON *result = reply ? cJSON_Duplicate(reply, 1) : cJSON_CreateObject();
    int ok = reply && kn_json_bool(reply, "ok", 0);
    if (!ok && !cJSON_GetObjectItemCaseSensitive(result, "err"))
        cJSON_AddStringToObject(result, "err", err ? err : "host failed");
    host_serve_store(hs, result, ok);
    cJSON_Delete(result);
}

static void on_host_claimed(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) {
        log_db_error("host request claim", res);
        return;
    }
    for (int i = 0; i < kn_db_rows(res); i++) {
        HostServe *hs = calloc(1, sizeof(*hs));
        if (!hs) continue;
        hs->id = kn_db_get_int64(res, i, 0, 0);
        snprintf(hs->from_brain, sizeof(hs->from_brain), "%s", kn_db_get(res, i, 1));
        const char *node = kn_db_get(res, i, 2);
        const char *cfg = kn_db_get(res, i, 3);
        cJSON *frame = cJSON_CreateObject();
        cJSON_AddStringToObject(frame, "type", "host");
        cJSON *config = kn_json_parse(cfg, strlen(cfg));
        cJSON_AddItemToObject(frame, "config", config ? config : cJSON_CreateObject());
        if (!kn_db_isnull(res, i, 4)) cJSON_AddNumberToObject(frame, "hostAccountId", (double) kn_db_get_int64(res, i, 4, 0));
        cJSON_AddStringToObject(frame, "hostName", kn_db_get(res, i, 5));
        if (internal_ws_request(g_b, node, frame, mm_settings()->host_timeout_s * 1000, on_served_host_reply, hs)) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddStringToObject(err, "err", "NodeGone");
            host_serve_store(hs, err, 0);
            cJSON_Delete(err);
        }
    }
}

static void claim_host_requests(void) {
    kn_db_call(g_b->db, &P_MM_HOST_CLAIM, KN_DB_ARGS(g_b->id), on_host_claimed, NULL);
}

static void on_notify_host(const char *channel, const char *payload, void *ud) {
    (void) ud;
    const char *colon = payload ? strchr(payload, ':') : NULL;
    if (!colon || strcmp(colon + 1, g_b->id)) return;
    if (!strcmp(channel, "kn_mm_host")) {
        claim_host_requests();
    } else {
        int64_t id = strtoll(payload, NULL, 10);
        for (HostWait *w = g_waits; w; w = w->next) if (w->id == id) check_wait(w);
    }
}

static void host_poll_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    /* fallback when NOTIFY is lost (listen connection reconnecting) */
    claim_host_requests();
    int64_t now = kn_wall_ms();
    for (HostWait *w = g_waits; w;) {
        HostWait *next = w->next;
        if (now > w->deadline_ms) wait_finish(w, NULL, "HostTimeout");
        else check_wait(w);
        w = next;
    }
}

static void on_notify_lobby(const char *channel, const char *payload, void *ud) {
    (void) channel;
    (void) ud;
    if (payload && !strcmp(payload, g_b->id)) return;
    refresh_start();
}

/* ---------------------------------------------------------------- lifecycle */

int federation_init(Brain *b) {
    g_b = b;
    g_active = mm_settings()->federation && b->db;
    snprintf(g_ttl_s, sizeof(g_ttl_s), "%d", mm_settings()->node_ttl_s);
    if (!g_active) {
        KN_INFO("federation", "disabled (%s): this brain only lists its own game servers",
                b->db ? "brain.federation=false" : "no database");
        return 0;
    }
    kn_db_listen(b->db, "kn_mm_lobby", on_notify_lobby, NULL);
    kn_db_listen(b->db, "kn_mm_host", on_notify_host, NULL);
    kn_db_listen(b->db, "kn_mm_host_done", on_notify_host, NULL);
    kn_db_listen(b->db, "kn_mm_assign", on_notify_assign, NULL);
    uint64_t hb = (uint64_t) mm_settings()->heartbeat_s * 1000;
    heartbeat_tick(b->loop, NULL);
    g_hb_timer = kn_loop_add_timer(b->loop, hb, hb, heartbeat_tick, NULL);
    g_refresh_timer = kn_loop_add_timer(b->loop, 50, REFRESH_MS, refresh_tick, NULL);
    g_host_poll_timer = kn_loop_add_timer(b->loop, HOST_POLL_MS, HOST_POLL_MS, host_poll_tick, NULL);
    KN_INFO("federation", "enabled: brain \"%s\" (region %s), node TTL %ds, heartbeat %ds", b->id, b->region,
            mm_settings()->node_ttl_s, mm_settings()->heartbeat_s);
    return 0;
}

void federation_shutdown(Brain *b) {
    if (!g_active) return;
    kn_timer_id *timers[] = {&g_hb_timer, &g_refresh_timer, &g_flush_timer, &g_host_poll_timer};
    for (size_t i = 0; i < sizeof(timers) / sizeof(*timers); i++) {
        if (*timers[i]) kn_loop_cancel_timer(b->loop, *timers[i]);
        *timers[i] = 0;
    }
    while (g_waits) wait_finish(g_waits, NULL, "updating");
    g_active = 0;
}
