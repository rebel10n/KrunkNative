/*
 * knsvc/db.h front end: picks the driver (database.driver), dispatches the
 * pool / transaction / prepared-statement / pub-sub API to its async ops or to
 * the worker pool (store_worker.c), resolves kn_sql texts and runs kn_procs,
 * and emulates publish/subscribe with the kn_pubsub table on drivers without it.
 */
#include "store_internal.h"
#include "knsvc/config.h"
#include "knsvc/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PUBSUB_KEEP_MS 60000
#define PUBSUB_LOOKBACK 64
#define PUBSUB_SEEN 512

typedef struct stmt {
    char *name, *sql;
} stmt;

typedef struct sub {
    char *channel;
    kn_db_notify_cb cb;
    void *ud;
} sub;

typedef struct pending_notify {
    char *channel, *payload;
    struct pending_notify *next;
} pending_notify;

struct kn_db {
    const kn_store_driver *drv;
    kn_loop *loop;
    kn_db_config cfg;
    char *strs[8];
    void *apool;          /* async driver pool */
    kn_wpool *wp;         /* blocking driver pool, or postgres' pool for C procs (lazy) */
    stmt *stmts;
    size_t nstmts;
    /* pub/sub emulation */
    sub *subs;
    size_t nsubs;
    int ps_state;         /* 0 unknown, 1 creating, 2 ready, 3 failed */
    int ps_polling, ps_inflight, ps_ticks;
    kn_timer_id ps_timer;
    int64_t ps_last;
    int64_t ps_seen[PUBSUB_SEEN];
    size_t ps_seen_pos;
    pending_notify *ps_pending;
    /* external transport (kn_db_set_pubsub) */
    const kn_db_pubsub_ops *xps;
    void *xps_ctx;
    int xps_slow_ms, xps_up, ps_cur_ms;
    int freeing;
};

struct kn_db_tx {
    kn_db *db;
    void *inner;          /* driver tx (async) or kn_wtx */
};

/* ------------------------------------------------------------------ config */

void kn_db_config_from(const kn_config *cfg, kn_db_config *o) {
    static char path[4096];
    memset(o, 0, sizeof *o);
    o->driver = kn_config_str(cfg, "database.driver", NULL);
    if (o->driver && !*o->driver) o->driver = NULL;
    if (!o->driver) {
        int network = kn_config_has(cfg, "database.host") || kn_config_has(cfg, "database.name") ||
                      kn_config_has(cfg, "database.user");
        const kn_store_driver *d = kn_store_default_driver(network);
        o->driver = d ? d->name : "postgres";
    }
    const kn_store_driver *drv = kn_store_driver_find(o->driver);
    o->host = kn_config_str(cfg, "database.host", "127.0.0.1");
    o->port = (int)kn_config_int(cfg, "database.port", drv && drv->default_port ? drv->default_port : 5432);
    o->name = kn_config_str(cfg, "database.name", "krunknative");
    o->user = kn_config_str(cfg, "database.user", "krunknative");
    o->password = kn_config_str(cfg, "database.password", "");
    o->search_path = kn_config_str(cfg, "database.search_path", "");
    if (o->search_path && !*o->search_path) o->search_path = NULL;
    o->application_name = kn_config_str(cfg, "database.application_name", NULL);
    o->pool_size = (int)kn_config_int(cfg, "database.pool_size", drv ? drv->default_pool : 4);
    o->connect_timeout_s = (int)kn_config_int(cfg, "database.connect_timeout_s", 5);
    o->statement_timeout_ms = (int)kn_config_int(cfg, "database.statement_timeout_ms", 0);
    o->notify_poll_ms = (int)kn_config_int(cfg, "database.notify_poll_ms", 100);
    o->busy_timeout_ms = (int)kn_config_int(cfg, "database.busy_timeout_ms", 5000);
    const char *p = kn_config_str(cfg, "database.path", "data/coordinator.db");
    if (p && (!strcmp(p, ":memory:") || !strncmp(p, "file:", 5))) o->path = p;
    else o->path = p && kn_config_resolve_path(cfg, p, path, sizeof path) ? path : p;
}

/* ------------------------------------------------------------------ pool */

static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }

static int wp_threads(const kn_db *db) {
    int n = db->cfg.pool_size > 0 ? db->cfg.pool_size : (db->drv->default_pool > 0 ? db->drv->default_pool : 2);
    if (db->cfg.path && !strcmp(db->drv->dialect, "sqlite") && !strcmp(db->cfg.path, ":memory:")) n = 1;
    return n;
}

kn_db *kn_db_new(kn_loop *loop, const kn_db_config *cfg) {
    const char *name = cfg->driver && *cfg->driver ? cfg->driver : NULL;
    const kn_store_driver *drv = name ? kn_store_driver_find(name) : kn_store_default_driver(1);
    if (!drv) {
        KN_ERROR("db", "unknown or not built-in database driver \"%s\" (built in:%s%s%s)", name ? name : "",
                 kn_store_driver_find("postgres") ? " postgres" : "", kn_store_driver_find("sqlite") ? " sqlite" : "",
                 kn_store_driver_find("mysql") ? " mysql" : "");
        return NULL;
    }
    kn_db *db = calloc(1, sizeof *db);
    if (!db) return NULL;
    db->drv = drv;
    db->loop = loop;
    db->cfg = *cfg;
    const char **fields[] = {&db->cfg.host, &db->cfg.name, &db->cfg.user, &db->cfg.password,
                             &db->cfg.search_path, &db->cfg.application_name, &db->cfg.driver, &db->cfg.path};
    for (int i = 0; i < 8; i++) {
        db->strs[i] = dup_or_null(*fields[i]);
        *fields[i] = db->strs[i];
    }
    db->cfg.driver = drv->name;
    if (db->cfg.port <= 0 && drv->default_port) db->cfg.port = drv->default_port;
    if (db->cfg.notify_poll_ms <= 0) db->cfg.notify_poll_ms = 100;
    if (drv->default_port)
        KN_INFO("db", "driver %s: %s@%s:%d/%s%s%s", drv->name, db->cfg.user ? db->cfg.user : "", db->cfg.host ? db->cfg.host : "",
                db->cfg.port, db->cfg.name ? db->cfg.name : "", db->cfg.search_path ? " schema " : "",
                db->cfg.search_path ? db->cfg.search_path : "");
    else
        KN_INFO("db", "driver %s: %s", drv->name, db->cfg.path ? db->cfg.path : "");
    if (drv->async) {
        db->apool = drv->async->pool_new(loop, &db->cfg);
        if (!db->apool) goto fail;
    } else {
        db->wp = kn_wpool_new(loop, drv, &db->cfg, wp_threads(db));
        if (!db->wp) goto fail;
    }
    return db;
fail:
    for (int i = 0; i < 8; i++) free(db->strs[i]);
    free(db);
    return NULL;
}

void kn_db_free(kn_db *db) {
    if (!db) return;
    db->freeing = 1;
    if (db->ps_timer) kn_loop_cancel_timer(db->loop, db->ps_timer);
    if (db->apool) db->drv->async->pool_free(db->apool);
    if (db->wp) kn_wpool_free(db->wp);
    for (size_t i = 0; i < db->nstmts; i++) {
        free(db->stmts[i].name);
        free(db->stmts[i].sql);
    }
    free(db->stmts);
    for (size_t i = 0; i < db->nsubs; i++) free(db->subs[i].channel);
    free(db->subs);
    while (db->ps_pending) {
        pending_notify *n = db->ps_pending;
        db->ps_pending = n->next;
        free(n->channel);
        free(n->payload);
        free(n);
    }
    for (int i = 0; i < 8; i++) free(db->strs[i]);
    free(db);
}

int kn_db_ready_count(const kn_db *db) {
    return db->apool ? db->drv->async->ready_count(db->apool) : kn_wpool_ready(db->wp);
}

size_t kn_db_queue_len(const kn_db *db) {
    return db->apool ? db->drv->async->queue_len(db->apool) : kn_wpool_queue_len(db->wp);
}

int kn_db_wait_ready(kn_db *db, int timeout_ms) {
    uint64_t end = kn_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    while (kn_db_ready_count(db) == 0) {
        uint64_t now = kn_now_ms();
        if (now >= end) return -1;
        kn_loop_run_once(db->loop, (int)(end - now > 20 ? 20 : end - now));
    }
    return 0;
}

const char *kn_db_driver_name(const kn_db *db) { return db ? db->drv->name : ""; }
const char *kn_db_dialect(const kn_db *db) { return db ? db->drv->dialect : ""; }
unsigned kn_db_caps(const kn_db *db) { return db ? db->drv->caps : 0; }

/* Worker pool for C procs on an async driver (lazy). */
static kn_wpool *proc_pool(kn_db *db) {
    if (!db->wp) {
        int n = db->cfg.pool_size > 1 ? 2 : 1;
        db->wp = kn_wpool_new(db->loop, db->drv, &db->cfg, n);
    }
    return db->wp;
}

/* Report an error to cb on the next loop iteration. */
typedef struct late_err {
    kn_db_cb cb;
    void *ud;
    char state[6];
    char msg[200];
} late_err;

static void late_err_cb(kn_loop *loop, void *ud) {
    late_err *e = ud;
    kn_db_result *r = kn_db_result_error(e->state, e->msg);
    if (e->cb) e->cb(r, e->ud);
    kn_db_result_free(r);
    free(e);
}

static int fail_later(kn_db *db, const char *state, const char *msg, kn_db_cb cb, void *ud) {
    late_err *e = calloc(1, sizeof *e);
    if (!e) return -1;
    e->cb = cb;
    e->ud = ud;
    snprintf(e->state, sizeof e->state, "%s", state);
    snprintf(e->msg, sizeof e->msg, "%s", msg);
    return kn_loop_post(db->loop, late_err_cb, e);
}

int kn_db_query(kn_db *db, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!db || !sql || db->freeing) return -1;
    if (db->apool) return db->drv->async->query(db->apool, sql, nparams, params, cb, ud);
    char *copy = strdup(sql);
    if (!copy) return -1;
    return kn_wpool_query(db->wp, copy, nparams, params, cb, ud);
}

int kn_db_query_sql(kn_db *db, const kn_sql *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!db || !sql || db->freeing) return -1;
    char *text = kn_sql_resolve(sql, db->drv);
    if (!text) return fail_later(db, "42883", "no SQL text for this database driver", cb, ud);
    int rc;
    if (db->apool) {
        rc = db->drv->async->query(db->apool, text, nparams, params, cb, ud);
        free(text);
    } else {
        rc = kn_wpool_query(db->wp, text, nparams, params, cb, ud);
    }
    return rc;
}

int kn_db_call(kn_db *db, const kn_proc *proc, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!db || !proc || db->freeing) return -1;
    if (proc->pg && (db->drv->caps & KN_DB_CAP_PROCS_SQL))
        return kn_db_query(db, proc->pg, nparams, params, cb, ud);
    if (!proc->run) return fail_later(db, "42883", "procedure has no implementation for this driver", cb, ud);
    kn_wpool *wp = db->apool ? proc_pool(db) : db->wp;
    if (!wp) return -1;
    return kn_wpool_call(wp, proc, nparams, params, cb, ud);
}

/* ------------------------------------------------------------------ prepared statements */

static const stmt *find_stmt(const kn_db *db, const char *name) {
    for (size_t i = 0; i < db->nstmts; i++)
        if (strcmp(db->stmts[i].name, name) == 0) return &db->stmts[i];
    return NULL;
}

int kn_db_prepare(kn_db *db, const char *name, const char *sql) {
    if (!db || !name || !sql) return -1;
    if (db->apool) return db->drv->async->prepare(db->apool, name, sql);
    if (find_stmt(db, name)) return -1;
    stmt *n = realloc(db->stmts, (db->nstmts + 1) * sizeof *n);
    if (!n) return -1;
    db->stmts = n;
    n[db->nstmts].name = strdup(name);
    n[db->nstmts].sql = strdup(sql);
    if (!n[db->nstmts].name || !n[db->nstmts].sql) return -1;
    db->nstmts++;
    return 0;
}

int kn_db_exec_prepared(kn_db *db, const char *name, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!db || !name || db->freeing) return -1;
    if (db->apool) return db->drv->async->exec_prepared(db->apool, name, nparams, params, cb, ud);
    const stmt *s = find_stmt(db, name);
    if (!s) return kn_wpool_fail_async(db->wp, "26000", "unknown prepared statement", cb, ud);
    char *copy = strdup(s->sql);
    return copy ? kn_wpool_query(db->wp, copy, nparams, params, cb, ud) : -1;
}

/* ------------------------------------------------------------------ transactions */

typedef struct begin_ctx {
    kn_db *db;
    kn_db_tx_cb cb;
    void *ud;
} begin_ctx;

static void begin_done(void *inner, kn_db_result *res, void *ud) {
    begin_ctx *b = ud;
    kn_db_tx *tx = NULL;
    if (inner) {
        tx = calloc(1, sizeof *tx);
        if (tx) {
            tx->db = b->db;
            tx->inner = inner;
        } else if (b->db->apool) {
            b->db->drv->async->tx_rollback(inner, NULL, NULL);
        } else {
            kn_wtx_end(inner, 0, NULL, NULL);
        }
    }
    if (b->cb) b->cb(tx, res, b->ud);
    free(b);
}

int kn_db_begin(kn_db *db, kn_db_tx_cb cb, void *ud) {
    if (!db || db->freeing) return -1;
    begin_ctx *b = calloc(1, sizeof *b);
    if (!b) return -1;
    b->db = db;
    b->cb = cb;
    b->ud = ud;
    int rc = db->apool ? db->drv->async->begin(db->apool, begin_done, b) : kn_wpool_begin(db->wp, begin_done, b);
    if (rc != 0) free(b);
    return rc;
}

int kn_db_tx_query(kn_db_tx *tx, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!tx || !sql) return -1;
    if (tx->db->apool) return tx->db->drv->async->tx_query(tx->inner, sql, nparams, params, cb, ud);
    char *copy = strdup(sql);
    return copy ? kn_wtx_query(tx->inner, copy, nparams, params, cb, ud) : -1;
}

int kn_db_tx_query_sql(kn_db_tx *tx, const kn_sql *sql, int nparams, const char *const *params, kn_db_cb cb,
                       void *ud) {
    if (!tx || !sql) return -1;
    char *text = kn_sql_resolve(sql, tx->db->drv);
    if (tx->db->apool) {
        if (!text) return fail_later(tx->db, "42883", "no SQL text for this database driver", cb, ud);
        int rc = tx->db->drv->async->tx_query(tx->inner, text, nparams, params, cb, ud);
        free(text);
        return rc;
    }
    return kn_wtx_query(tx->inner, text, nparams, params, cb, ud);
}

int kn_db_tx_call(kn_db_tx *tx, const kn_proc *proc, int nparams, const char *const *params, kn_db_cb cb,
                  void *ud) {
    if (!tx || !proc) return -1;
    kn_db *db = tx->db;
    if (proc->pg && (db->drv->caps & KN_DB_CAP_PROCS_SQL)) return kn_db_tx_query(tx, proc->pg, nparams, params, cb, ud);
    if (db->apool)
        return fail_later(db, "0A000", "C procedures cannot join a transaction of an async driver", cb, ud);
    return kn_wtx_call(tx->inner, proc, nparams, params, cb, ud);
}

int kn_db_tx_exec_prepared(kn_db_tx *tx, const char *name, int nparams, const char *const *params, kn_db_cb cb,
                           void *ud) {
    if (!tx || !name) return -1;
    if (tx->db->apool) return tx->db->drv->async->tx_exec_prepared(tx->inner, name, nparams, params, cb, ud);
    const stmt *s = find_stmt(tx->db, name);
    if (!s) return kn_wtx_query(tx->inner, NULL, nparams, params, cb, ud);
    char *copy = strdup(s->sql);
    return copy ? kn_wtx_query(tx->inner, copy, nparams, params, cb, ud) : -1;
}

static int tx_end(kn_db_tx *tx, int commit, kn_db_cb cb, void *ud) {
    if (!tx) return -1;
    kn_db *db = tx->db;
    void *inner = tx->inner;
    free(tx);
    if (db->apool)
        return commit ? db->drv->async->tx_commit(inner, cb, ud) : db->drv->async->tx_rollback(inner, cb, ud);
    return kn_wtx_end(inner, commit, cb, ud);
}

int kn_db_tx_commit(kn_db_tx *tx, kn_db_cb cb, void *ud) { return tx_end(tx, 1, cb, ud); }
int kn_db_tx_rollback(kn_db_tx *tx, kn_db_cb cb, void *ud) { return tx_end(tx, 0, cb, ud); }

/* ------------------------------------------------------------------ pub/sub emulation */

static const kn_sql PS_DDL = {
    .any = "CREATE TABLE IF NOT EXISTS kn_pubsub (id {bigserial_pk}, channel VARCHAR(64) NOT NULL,"
           " payload TEXT NOT NULL, at_ms BIGINT NOT NULL)"};
static const kn_sql PS_MAX = {.any = "SELECT coalesce(max(id), 0) FROM kn_pubsub"};
static const kn_sql PS_INSERT = {.any = "INSERT INTO kn_pubsub (channel, payload, at_ms) VALUES ($1, $2, {int:$3})"};
static const kn_sql PS_POLL = {
    .any = "SELECT id, channel, payload FROM kn_pubsub WHERE id > {int:$1} ORDER BY id"};
static const kn_sql PS_PRUNE = {.any = "DELETE FROM kn_pubsub WHERE at_ms < {int:$1}"};

static void ps_ignore(kn_db_result *res, void *ud) {
    if (!kn_db_ok(res)) KN_WARN("db", "pub/sub %s: %s", (const char *)ud, kn_db_error(res));
}

static void ps_send(kn_db *db, const char *channel, const char *payload) {
    char now[32];
    snprintf(now, sizeof now, "%lld", (long long)kn_wall_ms());
    kn_db_query_sql(db, &PS_INSERT, KN_DB_ARGS(channel, payload, now), ps_ignore, "notify");
}

static int ps_seen(kn_db *db, int64_t id) {
    for (size_t i = 0; i < PUBSUB_SEEN; i++)
        if (db->ps_seen[i] == id) return 1;
    db->ps_seen[db->ps_seen_pos++ % PUBSUB_SEEN] = id;
    return 0;
}

static void ps_polled(kn_db_result *res, void *ud) {
    kn_db *db = ud;
    db->ps_inflight = 0;
    if (db->freeing) return;
    if (!kn_db_ok(res)) {
        ps_ignore(res, "poll");
        return;
    }
    for (int i = 0; i < kn_db_rows(res); i++) {
        int64_t id = kn_db_get_int64(res, i, 0, 0);
        if (id > db->ps_last) db->ps_last = id;
        if (ps_seen(db, id)) continue;
        const char *ch = kn_db_get(res, i, 1);
        const char *payload = kn_db_get(res, i, 2);
        for (size_t k = 0; k < db->nsubs; k++)
            if (!strcmp(db->subs[k].channel, ch)) db->subs[k].cb(ch, payload, db->subs[k].ud);
    }
}

static void ps_tick(kn_loop *loop, void *ud) {
    kn_db *db = ud;
    if (db->ps_state != 2 || db->ps_inflight) return;
    db->ps_inflight = 1;
    char from[32];
    /* look back a little: auto-increment ids may commit out of order on mysql */
    snprintf(from, sizeof from, "%lld", (long long)(db->ps_last > PUBSUB_LOOKBACK ? db->ps_last - PUBSUB_LOOKBACK : 0));
    kn_db_query_sql(db, &PS_POLL, KN_DB_ARGS(from), ps_polled, db);
    if (++db->ps_ticks % 600 == 0) { /* prune about once a minute */
        char old[32];
        snprintf(old, sizeof old, "%lld", (long long)(kn_wall_ms() - PUBSUB_KEEP_MS));
        kn_db_query_sql(db, &PS_PRUNE, KN_DB_ARGS(old), ps_ignore, "prune");
    }
}

static int ps_interval(const kn_db *db) {
    return db->xps && db->xps_up && db->xps_slow_ms > db->cfg.notify_poll_ms ? db->xps_slow_ms : db->cfg.notify_poll_ms;
}

static void ps_start_polling(kn_db *db) {
    if (db->ps_polling || db->ps_state != 2 || !db->nsubs) return;
    db->ps_polling = 1;
    db->ps_cur_ms = ps_interval(db);
    uint64_t ms = (uint64_t)db->ps_cur_ms;
    db->ps_timer = kn_loop_add_timer(db->loop, ms, ms, ps_tick, db);
}

static void ps_max_done(kn_db_result *res, void *ud) {
    kn_db *db = ud;
    if (db->freeing) return;
    if (kn_db_ok(res)) db->ps_last = kn_db_get_int64(res, 0, 0, 0);
    db->ps_state = 2;
    while (db->ps_pending) {
        pending_notify *n = db->ps_pending;
        db->ps_pending = n->next;
        ps_send(db, n->channel, n->payload);
        free(n->channel);
        free(n->payload);
        free(n);
    }
    ps_start_polling(db);
}

static void ps_ddl_done(kn_db_result *res, void *ud) {
    kn_db *db = ud;
    if (db->freeing) return;
    if (!kn_db_ok(res)) {
        KN_WARN("db", "pub/sub table: %s (retrying)", kn_db_error(res));
        db->ps_state = 0;
        return;
    }
    kn_db_query_sql(db, &PS_MAX, 0, NULL, ps_max_done, db);
}

static void ps_ensure(kn_db *db) {
    if (db->ps_state != 0) return;
    db->ps_state = 1;
    kn_db_query_sql(db, &PS_DDL, 0, NULL, ps_ddl_done, db);
}

int kn_db_set_pubsub(kn_db *db, const kn_db_pubsub_ops *ops, void *ctx, int slow_poll_ms) {
    if (!db || db->freeing || db->nsubs || db->xps) return -1;
    db->xps = ops;
    db->xps_ctx = ctx;
    db->xps_slow_ms = slow_poll_ms > 0 ? slow_poll_ms : 1000;
    return 0;
}

void kn_db_pubsub_transport_up(kn_db *db, int up) {
    if (!db || !db->xps || db->freeing) return;
    db->xps_up = up ? 1 : 0;
    if (db->ps_polling && db->ps_cur_ms != ps_interval(db)) { /* re-arm the poll at the new pace */
        kn_loop_cancel_timer(db->loop, db->ps_timer);
        db->ps_polling = 0;
        ps_start_polling(db);
    }
}

int kn_db_listen(kn_db *db, const char *channel, kn_db_notify_cb cb, void *ud) {
    if (!db || !channel || !cb || db->freeing) return -1;
    if (db->xps && db->xps->subscribe) db->xps->subscribe(db->xps_ctx, channel, cb, ud);
    if (db->apool && db->drv->async->listen && (db->drv->caps & KN_DB_CAP_NOTIFY))
        return db->drv->async->listen(db->apool, channel, cb, ud);
    sub *n = realloc(db->subs, (db->nsubs + 1) * sizeof *n);
    if (!n) return -1;
    db->subs = n;
    n[db->nsubs].channel = strdup(channel);
    n[db->nsubs].cb = cb;
    n[db->nsubs].ud = ud;
    if (!n[db->nsubs].channel) return -1;
    db->nsubs++;
    ps_ensure(db);
    ps_start_polling(db);
    return 0;
}

int kn_db_notify(kn_db *db, const char *channel, const char *payload) {
    if (!db || !channel || db->freeing) return -1;
    if (db->xps && db->xps->publish && db->xps->publish(db->xps_ctx, channel, payload ? payload : "") == 0) return 0;
    if (db->apool && db->drv->async->notify && (db->drv->caps & KN_DB_CAP_NOTIFY))
        return db->drv->async->notify(db->apool, channel, payload);
    if (db->ps_state == 2) {
        ps_send(db, channel, payload ? payload : "");
        return 0;
    }
    pending_notify *p = calloc(1, sizeof *p);
    if (!p) return -1;
    p->channel = strdup(channel);
    p->payload = strdup(payload ? payload : "");
    pending_notify **tail = &db->ps_pending;
    while (*tail) tail = &(*tail)->next;
    *tail = p;
    ps_ensure(db);
    return 0;
}

/* ------------------------------------------------------------------ synchronous connection */

kn_dbconn *kn_dbconn_open(const kn_db_config *cfg, char *err, size_t errlen) {
    const kn_store_driver *drv =
        cfg->driver && *cfg->driver ? kn_store_driver_find(cfg->driver) : kn_store_default_driver(1);
    if (!drv) {
        if (err && errlen) snprintf(err, errlen, "unknown or not built-in database driver \"%s\"", cfg->driver ? cfg->driver : "");
        return NULL;
    }
    kn_db_config c = *cfg;
    if (c.port <= 0 && drv->default_port) c.port = drv->default_port;
    void *conn = drv->blocking->connect(&c, err, errlen);
    if (!conn) return NULL;
    kn_dbconn *dc = calloc(1, sizeof *dc);
    if (!dc) {
        drv->blocking->disconnect(conn);
        return NULL;
    }
    dc->drv = drv;
    dc->conn = conn;
    dc->owned = 1;
    return dc;
}

void kn_dbconn_close(kn_dbconn *c) {
    if (!c) return;
    if (c->owned && c->conn) c->drv->blocking->disconnect(c->conn);
    free(c);
}

kn_db_result *kn_dbconn_exec(kn_dbconn *c, const char *sql, int nparams, const char *const *params) {
    if (!c || !c->conn) return kn_db_result_error("08003", "connection closed");
    kn_db_result *r = c->drv->blocking->exec(c->conn, sql, nparams, params);
    return r ? r : kn_db_result_error("53200", "out of memory");
}

kn_db_result *kn_dbconn_exec_sql(kn_dbconn *c, const kn_sql *sql, int nparams, const char *const *params) {
    if (!c) return kn_db_result_error("08003", "connection closed");
    char *text = kn_sql_resolve(sql, c->drv);
    if (!text) return kn_db_result_error("42883", "no SQL text for this database driver");
    kn_db_result *r = kn_dbconn_exec(c, text, nparams, params);
    free(text);
    return r;
}

kn_db_result *kn_dbconn_call(kn_dbconn *c, const kn_proc *proc, int nparams, const char *const *params) {
    if (!c || !proc) return kn_db_result_error("08003", "connection closed");
    if (proc->pg && (c->drv->caps & KN_DB_CAP_PROCS_SQL)) return kn_dbconn_exec(c, proc->pg, nparams, params);
    if (!proc->run) return kn_db_result_error("42883", "procedure has no implementation for this driver");
    kn_db_result *r = proc->run(c, nparams, params);
    return r ? r : kn_db_result_error("53200", "out of memory");
}

int kn_dbconn_exec_script(kn_dbconn *c, const char *sql, char *err, size_t errlen) {
    if (!c || !c->conn) {
        if (err && errlen) snprintf(err, errlen, "connection closed");
        return -1;
    }
    return c->drv->blocking->exec_script(c->conn, sql, err, errlen);
}

const char *kn_dbconn_driver_name(const kn_dbconn *c) { return c ? c->drv->name : ""; }
const char *kn_dbconn_dialect(const kn_dbconn *c) { return c ? c->drv->dialect : ""; }

void *kn_dbconn_native(kn_dbconn *c) {
    return c && c->conn && c->drv->blocking->native ? c->drv->blocking->native(c->conn) : NULL;
}

int64_t kn_dbconn_scalar_int(kn_dbconn *c, const kn_sql *sql, int nparams, const char *const *params, int64_t def) {
    kn_db_result *r = kn_dbconn_exec_sql(c, sql, nparams, params);
    int64_t v = kn_db_ok(r) && kn_db_rows(r) > 0 ? kn_db_get_int64(r, 0, 0, def) : def;
    kn_db_result_free(r);
    return v;
}

kn_db_result *kn_dbconn_try(kn_dbconn *c, const kn_sql *sql, int nparams, const char *const *params,
                            kn_db_result **err) {
    kn_db_result *r = kn_dbconn_exec_sql(c, sql, nparams, params);
    if (kn_db_ok(r)) return r;
    if (err && !*err) *err = r;
    else kn_db_result_free(r);
    return NULL;
}
