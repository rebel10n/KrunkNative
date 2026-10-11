/*
 * knsvc/db.h async pool over libpq.
 *
 * Each pool connection is a small state machine driven by the loop:
 *   DOWN -> CONNECTING (PQconnectPoll) -> IDLE <-> BUSY ... -> DOWN (backoff, reconnect)
 * Queries wait in the pool queue (or in a transaction's / the listener's own
 * queue) and run one at a time per connection. Prepared statements are
 * registered on the pool and prepared lazily per connection.
 */
#include "db_internal.h"
#include "knsvc/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKOFF_MIN_MS 250
#define BACKOFF_MAX_MS 10000

enum { C_DOWN, C_CONNECTING, C_IDLE, C_BUSY };
enum { Q_SQL, Q_PREPARED, Q_BEGIN, Q_END, Q_LISTEN };

typedef struct query {
    int kind;
    char *sql;            /* SQL text, or statement name for Q_PREPARED */
    int nparams;
    char **params;
    kn_db_cb cb;
    kn_db_tx_cb tx_cb;    /* Q_BEGIN */
    void *ud;
    kn_db_tx *tx;         /* Q_END: transaction being finished */
    struct query *next;
} query;

typedef struct qlist {
    query *head, *tail;
    size_t len;
} qlist;

typedef struct pgc {
    kn_db *db;
    PGconn *pg;
    int fd;
    int state;
    int listener;
    query *cur;
    int preparing;        /* cur is waiting for its PQsendPrepare */
    PGresult *last;       /* last result of cur */
    kn_db_tx *tx;         /* pinned transaction */
    char **prepared;
    size_t nprepared;
    kn_timer_id retry;
    int backoff_ms;
    qlist own;            /* listener: LISTEN commands */
} pgc;

struct kn_db_tx {
    kn_db *db;
    pgc *conn;            /* NULL once broken */
    qlist q;
    int ending;
};

typedef struct stmt {
    char *name, *sql;
} stmt;

typedef struct listen_sub {
    char *channel;
    kn_db_notify_cb cb;
    void *ud;
} listen_sub;

struct kn_db {
    kn_loop *loop;
    kn_db_config cfg;
    char *strs[7];        /* owned copies of cfg strings */
    pgc *conns;
    int nconns;
    pgc listener;
    int has_listener;
    qlist queue;
    stmt *stmts;
    size_t nstmts;
    listen_sub *subs;
    size_t nsubs;
    int freeing;
    int logged_down;
};

static void conn_start(pgc *c);
static void conn_next(pgc *c);
static void conn_io(kn_loop *loop, int fd, unsigned ev, void *ud);

/* ------------------------------------------------------------------ queues / queries */

static void q_push(qlist *q, query *x) {
    x->next = NULL;
    if (q->tail) q->tail->next = x;
    else q->head = x;
    q->tail = x;
    q->len++;
}

static query *q_pop(qlist *q) {
    query *x = q->head;
    if (!x) return NULL;
    q->head = x->next;
    if (!q->head) q->tail = NULL;
    q->len--;
    x->next = NULL;
    return x;
}

static void query_free(query *q) {
    if (!q) return;
    for (int i = 0; i < q->nparams; i++) free(q->params[i]);
    free(q->params);
    free(q->sql);
    free(q);
}

static query *query_new(int kind, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    query *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->kind = kind;
    q->cb = cb;
    q->ud = ud;
    q->sql = sql ? strdup(sql) : NULL;
    if (sql && !q->sql) goto oom;
    if (nparams > 0) {
        q->params = calloc((size_t)nparams, sizeof *q->params);
        if (!q->params) goto oom;
        q->nparams = nparams;
        for (int i = 0; i < nparams; i++) {
            if (!params[i]) continue; /* SQL NULL */
            q->params[i] = strdup(params[i]);
            if (!q->params[i]) goto oom;
        }
    }
    return q;
oom:
    query_free(q);
    return NULL;
}

static void tx_free(kn_db_tx *tx);

/* Deliver res for q (takes ownership of res), free q. */
static void query_complete(kn_db *db, query *q, kn_db_result *res) {
    if (q->kind == Q_BEGIN) {
        if (q->tx_cb) q->tx_cb(kn_db_ok(res) ? q->tx : NULL, res, q->ud);
        if (!kn_db_ok(res) && q->tx) tx_free(q->tx);
    } else if (q->cb) {
        q->cb(res, q->ud);
    }
    kn_db_result_free(res);
    query_free(q);
}

static void query_fail(kn_db *db, query *q, const char *state, const char *msg) {
    if (q->kind == Q_END && q->tx) {
        kn_db_tx *tx = q->tx;
        q->tx = NULL;
        tx_free(tx);
    }
    query_complete(db, q, kn_db__result_error(state, msg));
}

/* ------------------------------------------------------------------ transactions */

static void tx_free(kn_db_tx *tx) {
    if (!tx) return;
    while (tx->q.head) {
        query *q = q_pop(&tx->q);
        query_fail(tx->db, q, "25P02", "transaction ended");
    }
    if (tx->conn) tx->conn->tx = NULL;
    free(tx);
}

/* The pinned connection died: fail the transaction's queued queries; keep the
 * tx object until the user commits/rolls back. */
static void tx_break(kn_db_tx *tx) {
    tx->conn = NULL;
    while (tx->q.head) {
        query *q = q_pop(&tx->q);
        query_fail(tx->db, q, "08006", "connection lost during transaction");
    }
}

/* ------------------------------------------------------------------ connection lifecycle */

static int conn_index(const pgc *c) {
    return c->listener ? -1 : (int)(c - c->db->conns);
}

static void conn_close_pg(pgc *c) {
    if (c->fd >= 0) kn_loop_del_fd(c->db->loop, c->fd);
    c->fd = -1;
    if (c->pg) PQfinish(c->pg);
    c->pg = NULL;
    PQclear(c->last);
    c->last = NULL;
    for (size_t i = 0; i < c->nprepared; i++) free(c->prepared[i]);
    free(c->prepared);
    c->prepared = NULL;
    c->nprepared = 0;
    c->state = C_DOWN;
}

static void retry_cb(kn_loop *loop, void *ud) {
    pgc *c = ud;
    c->retry = 0;
    conn_start(c);
}

/* Connection failed or broke: fail its in-flight work and schedule a reconnect. */
static void conn_lost(pgc *c, const char *why_raw) {
    kn_db *db = c->db;
    char why[512];
    size_t n = 0;
    for (const char *p = why_raw ? why_raw : "unknown error"; *p && n + 1 < sizeof why; p++) {
        char ch = (*p == '\n' || *p == '\t' || *p == '\r') ? ' ' : *p;
        if (ch == ' ' && (n == 0 || why[n - 1] == ' ')) continue;
        why[n++] = ch;
    }
    while (n && why[n - 1] == ' ') n--;
    why[n] = 0;
    if (!db->logged_down || c->state != C_CONNECTING)
        KN_WARN("db", "%s connection %d lost: %s", c->listener ? "listener" : "pool", conn_index(c), why);
    db->logged_down = 1;
    query *cur = c->cur;
    c->cur = NULL;
    c->preparing = 0;
    conn_close_pg(c);
    if (cur) query_fail(db, cur, "08006", why);
    if (c->tx) {
        kn_db_tx *tx = c->tx;
        c->tx = NULL;
        tx_break(tx);
    }
    if (db->freeing) return;
    c->backoff_ms = c->backoff_ms ? c->backoff_ms * 2 : BACKOFF_MIN_MS;
    if (c->backoff_ms > BACKOFF_MAX_MS) c->backoff_ms = BACKOFF_MAX_MS;
    c->retry = kn_loop_add_timer(db->loop, (uint64_t)c->backoff_ms, 0, retry_cb, c);
}

static void conn_watch(pgc *c, unsigned ev) {
    int fd = PQsocket(c->pg);
    if (fd != c->fd) {
        if (c->fd >= 0) kn_loop_del_fd(c->db->loop, c->fd);
        c->fd = fd;
        if (fd >= 0 && kn_loop_add_fd(c->db->loop, fd, ev, conn_io, c) != 0) c->fd = -1;
        return;
    }
    if (fd >= 0) kn_loop_mod_fd(c->db->loop, fd, ev);
}

static void quiet_notice(void *arg, const PGresult *res) { (void)arg; (void)res; }

static void conn_start(pgc *c) {
    kn_db__conninfo ci;
    kn_db__conninfo_build(&c->db->cfg, &ci);
    c->pg = PQconnectStartParams(ci.keys, ci.vals, 0);
    if (!c->pg || PQstatus(c->pg) == CONNECTION_BAD) {
        conn_lost(c, c->pg ? PQerrorMessage(c->pg) : "out of memory");
        return;
    }
    PQsetNoticeReceiver(c->pg, quiet_notice, NULL);
    c->state = C_CONNECTING;
    conn_watch(c, KN_EV_WRITE);
}

static void conn_connect_step(pgc *c) {
    switch (PQconnectPoll(c->pg)) {
    case PGRES_POLLING_READING: conn_watch(c, KN_EV_READ); return;
    case PGRES_POLLING_WRITING: conn_watch(c, KN_EV_WRITE); return;
    case PGRES_POLLING_OK: break;
    default: {
        char msg[512];
        snprintf(msg, sizeof msg, "%s", PQerrorMessage(c->pg));
        size_t n = strlen(msg);
        while (n && msg[n - 1] == '\n') msg[--n] = 0;
        conn_lost(c, msg);
        return;
    }
    }
    PQsetnonblocking(c->pg, 1);
    c->state = C_IDLE;
    c->backoff_ms = 0;
    if (c->db->logged_down) KN_INFO("db", "connection %d up", conn_index(c));
    c->db->logged_down = 0;
    if (c->listener) {
        /* (re-)LISTEN every subscribed channel */
        for (size_t i = 0; i < c->db->nsubs; i++) {
            char *id = PQescapeIdentifier(c->pg, c->db->subs[i].channel, strlen(c->db->subs[i].channel));
            if (!id) continue;
            char sql[600];
            snprintf(sql, sizeof sql, "LISTEN %s", id);
            PQfreemem(id);
            query *q = query_new(Q_LISTEN, sql, 0, NULL, NULL, NULL);
            if (q) q_push(&c->own, q);
        }
    }
    conn_watch(c, KN_EV_READ);
    conn_next(c);
}

/* ------------------------------------------------------------------ query execution */

static int conn_has_prepared(const pgc *c, const char *name) {
    for (size_t i = 0; i < c->nprepared; i++)
        if (strcmp(c->prepared[i], name) == 0) return 1;
    return 0;
}

static const stmt *find_stmt(const kn_db *db, const char *name) {
    for (size_t i = 0; i < db->nstmts; i++)
        if (strcmp(db->stmts[i].name, name) == 0) return &db->stmts[i];
    return NULL;
}

static int conn_send(pgc *c, query *q) {
    const char *const *params = (const char *const *)q->params;
    switch (q->kind) {
    case Q_SQL:
    case Q_LISTEN:
        return PQsendQueryParams(c->pg, q->sql, q->nparams, NULL, params, NULL, NULL, 0);
    case Q_BEGIN:
        return PQsendQuery(c->pg, "BEGIN");
    case Q_END:
        return PQsendQuery(c->pg, q->sql);
    case Q_PREPARED: {
        if (!conn_has_prepared(c, q->sql)) {
            const stmt *s = find_stmt(c->db, q->sql);
            if (!s) return -2;
            c->preparing = 1;
            return PQsendPrepare(c->pg, s->name, s->sql, 0, NULL);
        }
        return PQsendQueryPrepared(c->pg, q->sql, q->nparams, params, NULL, NULL, 0);
    }
    }
    return 0;
}

static query *conn_take_work(pgc *c) {
    if (c->listener) return q_pop(&c->own);
    if (c->tx) return q_pop(&c->tx->q);
    return q_pop(&c->db->queue);
}

static void conn_flush_out(pgc *c) {
    int r = PQflush(c->pg);
    if (r < 0) conn_lost(c, PQerrorMessage(c->pg));
    else conn_watch(c, KN_EV_READ | (r == 1 ? KN_EV_WRITE : 0u));
}

/* Start the next queued query on an idle connection. */
static void conn_next(pgc *c) {
    while (c->state == C_IDLE && !c->cur) {
        query *q = conn_take_work(c);
        if (!q) return;
        int r = conn_send(c, q);
        if (r == -2) {
            query_fail(c->db, q, "26000", "unknown prepared statement");
            continue;
        }
        if (r != 1) {
            char msg[512];
            snprintf(msg, sizeof msg, "%s", PQerrorMessage(c->pg));
            /* put it back: the reconnect will retry it */
            if (c->tx || c->listener) query_fail(c->db, q, "08006", msg);
            else {
                q->next = c->db->queue.head;
                c->db->queue.head = q;
                if (!c->db->queue.tail) c->db->queue.tail = q;
                c->db->queue.len++;
            }
            conn_lost(c, msg);
            return;
        }
        c->cur = q;
        c->state = C_BUSY;
        conn_flush_out(c);
    }
}

/* Find an idle pool connection for newly queued work. */
static void pool_kick(kn_db *db) {
    for (int i = 0; i < db->nconns && db->queue.head; i++) {
        pgc *c = &db->conns[i];
        if (c->state == C_IDLE && !c->cur && !c->tx) conn_next(c);
    }
}

static void deliver_notifies(pgc *c) {
    PGnotify *n;
    while (c->pg && (n = PQnotifies(c->pg)) != NULL) {
        for (size_t i = 0; i < c->db->nsubs; i++)
            if (strcmp(c->db->subs[i].channel, n->relname) == 0)
                c->db->subs[i].cb(n->relname, n->extra ? n->extra : "", c->db->subs[i].ud);
        PQfreemem(n);
    }
}

/* All results of cur received. */
static void conn_query_done(pgc *c) {
    kn_db *db = c->db;
    query *q = c->cur;
    PGresult *last = c->last;
    c->last = NULL;
    if (c->preparing) {
        /* the PQsendPrepare finished; now run the statement itself */
        c->preparing = 0;
        ExecStatusType st = last ? PQresultStatus(last) : PGRES_FATAL_ERROR;
        if (st != PGRES_COMMAND_OK) {
            c->cur = NULL;
            c->state = C_IDLE;
            query_complete(db, q, kn_db__result_from_pg(last, c->pg));
            conn_next(c);
            pool_kick(db);
            return;
        }
        PQclear(last);
        char **n = realloc(c->prepared, (c->nprepared + 1) * sizeof *n);
        if (n) {
            c->prepared = n;
            c->prepared[c->nprepared] = strdup(q->sql);
            if (c->prepared[c->nprepared]) c->nprepared++;
        }
        if (PQsendQueryPrepared(c->pg, q->sql, q->nparams, (const char *const *)q->params, NULL, NULL, 0) != 1) {
            conn_lost(c, PQerrorMessage(c->pg));
            return;
        }
        conn_flush_out(c);
        return;
    }
    c->cur = NULL;
    c->state = C_IDLE;
    kn_db_result *res = kn_db__result_from_pg(last, c->pg);
    if (q->kind == Q_BEGIN && kn_db_ok(res)) {
        kn_db_tx *tx = calloc(1, sizeof *tx);
        if (tx) {
            tx->db = db;
            tx->conn = c;
            c->tx = tx;
        }
        q->tx = tx;
        if (!tx) {
            kn_db_result_free(res);
            res = kn_db__result_error("53200", "out of memory");
        }
    }
    if (q->kind == Q_END && kn_db_ok(res) && strcmp(q->sql, "COMMIT") == 0 && last &&
        strcmp(PQcmdStatus(last), "ROLLBACK") == 0) {
        /* COMMIT of an aborted transaction: PostgreSQL rolled back */
        kn_db_result_free(res);
        res = kn_db__result_error("40000", "transaction rolled back: a statement in it failed");
    }
    if (q->kind == Q_END) {
        kn_db_tx *tx = q->tx;
        q->tx = NULL;
        if (tx) {
            c->tx = NULL;
            tx->conn = NULL;
            tx_free(tx);
        }
    }
    query_complete(db, q, res);
    if (db->freeing) return;
    conn_next(c);
    pool_kick(db);
}

static void conn_read(pgc *c) {
    if (!PQconsumeInput(c->pg)) {
        conn_lost(c, PQerrorMessage(c->pg));
        return;
    }
    while (c->cur && !PQisBusy(c->pg)) {
        PGresult *r = PQgetResult(c->pg);
        if (!r) {
            conn_query_done(c);
            if (c->state != C_BUSY) break;
            continue;
        }
        PQclear(c->last);
        c->last = r;
    }
    deliver_notifies(c);
    if (c->pg && PQstatus(c->pg) == CONNECTION_BAD) conn_lost(c, "server closed the connection");
}

static void conn_io(kn_loop *loop, int fd, unsigned ev, void *ud) {
    pgc *c = ud;
    if (c->state == C_CONNECTING) {
        conn_connect_step(c);
        return;
    }
    if (!c->pg) return;
    if (ev & KN_EV_WRITE) {
        conn_flush_out(c);
        if (!c->pg) return;
    }
    if (ev & (KN_EV_READ | KN_EV_HUP | KN_EV_ERROR)) conn_read(c);
}

/* ------------------------------------------------------------------ public: pool */

static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }

kn_db *kn_db_new(kn_loop *loop, const kn_db_config *cfg) {
    kn_db *db = calloc(1, sizeof *db);
    if (!db) return NULL;
    db->loop = loop;
    db->cfg = *cfg;
    const char **fields[] = {&db->cfg.host, &db->cfg.name, &db->cfg.user, &db->cfg.password,
                             &db->cfg.search_path, &db->cfg.application_name};
    for (int i = 0; i < 6; i++) {
        db->strs[i] = dup_or_null(*fields[i]);
        *fields[i] = db->strs[i];
    }
    db->nconns = cfg->pool_size > 0 ? cfg->pool_size : 4;
    db->conns = calloc((size_t)db->nconns, sizeof *db->conns);
    if (!db->conns) {
        free(db);
        return NULL;
    }
    for (int i = 0; i < db->nconns; i++) {
        db->conns[i].db = db;
        db->conns[i].fd = -1;
        conn_start(&db->conns[i]);
    }
    db->listener.db = db;
    db->listener.fd = -1;
    db->listener.listener = 1;
    return db;
}

static void fail_list(kn_db *db, qlist *q) {
    query *x;
    while ((x = q_pop(q)) != NULL) query_fail(db, x, "08003", "database pool closed");
}

static void conn_shutdown(pgc *c) {
    kn_db *db = c->db;
    kn_loop_cancel_timer(db->loop, c->retry);
    c->retry = 0;
    query *cur = c->cur;
    c->cur = NULL;
    conn_close_pg(c);
    if (cur) query_fail(db, cur, "08003", "database pool closed");
    fail_list(db, &c->own);
}

void kn_db_free(kn_db *db) {
    if (!db) return;
    db->freeing = 1;
    fail_list(db, &db->queue);
    for (int i = 0; i < db->nconns; i++) conn_shutdown(&db->conns[i]);
    conn_shutdown(&db->listener);
    for (size_t i = 0; i < db->nstmts; i++) {
        free(db->stmts[i].name);
        free(db->stmts[i].sql);
    }
    free(db->stmts);
    for (size_t i = 0; i < db->nsubs; i++) free(db->subs[i].channel);
    free(db->subs);
    for (int i = 0; i < 7; i++) free(db->strs[i]);
    free(db->conns);
    free(db);
}

int kn_db_ready_count(const kn_db *db) {
    int n = 0;
    for (int i = 0; i < db->nconns; i++) n += db->conns[i].state == C_IDLE || db->conns[i].state == C_BUSY;
    return n;
}

size_t kn_db_queue_len(const kn_db *db) { return db->queue.len; }

int kn_db_wait_ready(kn_db *db, int timeout_ms) {
    uint64_t end = kn_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    while (kn_db_ready_count(db) == 0) {
        uint64_t now = kn_now_ms();
        if (now >= end) return -1;
        kn_loop_run_once(db->loop, (int)(end - now > 50 ? 50 : end - now));
    }
    return 0;
}

static int enqueue(kn_db *db, query *q) {
    if (!q) return -1;
    q_push(&db->queue, q);
    pool_kick(db);
    return 0;
}

int kn_db_query(kn_db *db, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!db || !sql || db->freeing) return -1;
    return enqueue(db, query_new(Q_SQL, sql, nparams, params, cb, ud));
}

int kn_db_prepare(kn_db *db, const char *name, const char *sql) {
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
    return enqueue(db, query_new(Q_PREPARED, name, nparams, params, cb, ud));
}

/* ------------------------------------------------------------------ public: transactions */

int kn_db_begin(kn_db *db, kn_db_tx_cb cb, void *ud) {
    if (!db || db->freeing) return -1;
    query *q = query_new(Q_BEGIN, NULL, 0, NULL, NULL, ud);
    if (!q) return -1;
    q->tx_cb = cb;
    return enqueue(db, q);
}

static void broken_cb(kn_loop *loop, void *ud) {
    query *q = ud;
    kn_db *db = q->tx ? q->tx->db : NULL;
    kn_db_tx *tx = q->kind == Q_END ? q->tx : NULL;
    q->tx = NULL;
    if (tx) tx_free(tx);
    query_complete(db, q, kn_db__result_error("08006", "transaction connection lost"));
}

static int tx_enqueue(kn_db_tx *tx, query *q) {
    if (!q) return -1;
    if (!tx->conn) {
        /* broken: report asynchronously like any other failure */
        q->tx = tx;
        return kn_loop_post(tx->db->loop, broken_cb, q);
    }
    q_push(&tx->q, q);
    if (tx->conn->state == C_IDLE && !tx->conn->cur) conn_next(tx->conn);
    return 0;
}

int kn_db_tx_query(kn_db_tx *tx, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!tx || tx->ending || !sql) return -1;
    query *q = query_new(Q_SQL, sql, nparams, params, cb, ud);
    if (q && !tx->conn) q->kind = Q_SQL;
    if (q && !tx->conn) {
        q->tx = tx;
        return kn_loop_post(tx->db->loop, broken_cb, q) ? -1 : 0;
    }
    return tx_enqueue(tx, q);
}

int kn_db_tx_exec_prepared(kn_db_tx *tx, const char *name, int nparams, const char *const *params, kn_db_cb cb,
                           void *ud) {
    if (!tx || tx->ending || !name) return -1;
    return tx_enqueue(tx, query_new(Q_PREPARED, name, nparams, params, cb, ud));
}

static int tx_end(kn_db_tx *tx, const char *sql, kn_db_cb cb, void *ud) {
    if (!tx || tx->ending) return -1;
    tx->ending = 1;
    query *q = query_new(Q_END, sql, 0, NULL, cb, ud);
    if (!q) return -1;
    q->tx = tx;
    if (!tx->conn) return kn_loop_post(tx->db->loop, broken_cb, q);
    q_push(&tx->q, q);
    if (tx->conn->state == C_IDLE && !tx->conn->cur) conn_next(tx->conn);
    return 0;
}

int kn_db_tx_commit(kn_db_tx *tx, kn_db_cb cb, void *ud) { return tx_end(tx, "COMMIT", cb, ud); }
int kn_db_tx_rollback(kn_db_tx *tx, kn_db_cb cb, void *ud) { return tx_end(tx, "ROLLBACK", cb, ud); }

/* ------------------------------------------------------------------ public: LISTEN/NOTIFY */

int kn_db_listen(kn_db *db, const char *channel, kn_db_notify_cb cb, void *ud) {
    if (!db || !channel || !cb || db->freeing) return -1;
    listen_sub *n = realloc(db->subs, (db->nsubs + 1) * sizeof *n);
    if (!n) return -1;
    db->subs = n;
    n[db->nsubs].channel = strdup(channel);
    n[db->nsubs].cb = cb;
    n[db->nsubs].ud = ud;
    if (!n[db->nsubs].channel) return -1;
    db->nsubs++;
    pgc *l = &db->listener;
    if (!db->has_listener) {
        db->has_listener = 1;
        conn_start(l); /* LISTENs every channel once connected */
        return 0;
    }
    if (l->state == C_IDLE || l->state == C_BUSY) {
        char *id = PQescapeIdentifier(l->pg, channel, strlen(channel));
        if (!id) return -1;
        char sql[600];
        snprintf(sql, sizeof sql, "LISTEN %s", id);
        PQfreemem(id);
        query *q = query_new(Q_LISTEN, sql, 0, NULL, NULL, NULL);
        if (!q) return -1;
        q_push(&l->own, q);
        conn_next(l);
    }
    return 0;
}

int kn_db_notify(kn_db *db, const char *channel, const char *payload) {
    return kn_db_query(db, "SELECT pg_notify($1, $2)", KN_DB_ARGS(channel, payload ? payload : ""), NULL, NULL);
}
