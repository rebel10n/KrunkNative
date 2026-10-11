/*
 * PostgreSQL driver (libpq) of the store layer: database.driver = "postgres".
 *
 * Async pool on the kn_loop: each pool connection is a small state machine
 * driven by the loop:
 *   DOWN -> CONNECTING (PQconnectPoll) -> IDLE <-> BUSY ... -> DOWN (backoff, reconnect)
 * Queries wait in the pool queue (or in a transaction's / the listener's own
 * queue) and run one at a time per connection. Prepared statements are
 * registered on the pool and prepared lazily per connection. LISTEN/NOTIFY on
 * a dedicated connection. Blocking ops (kn_dbconn, C kn_procs) use PQexec*.
 * Server-side SQL functions come from services/db/migrations/postgres.
 */
#include "../store_internal.h"
#include "knsvc/log.h"

#include <libpq-fe.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pg_pool pg_pool;
typedef struct pg_tx pg_tx;

/* ------------------------------------------------------------------ results */

/* PostgreSQL type OIDs (catalog/pg_type.h) */
#define OID_BOOL 16
#define OID_BYTEA 17
#define OID_INT8 20
#define OID_INT2 21
#define OID_INT4 23
#define OID_OID 26
#define OID_JSON 114
#define OID_FLOAT4 700
#define OID_FLOAT8 701
#define OID_NUMERIC 1700
#define OID_JSONB 3802

static int r_rows(const void *n) { return PQntuples(n); }
static int r_cols(const void *n) { return PQnfields(n); }
static const char *r_colname(const void *n, int c) { return PQfname(n, c); }
static int r_colnum(const void *n, const char *name) { return PQfnumber(n, name); }
static int r_isnull(const void *n, int row, int c) { return PQgetisnull(n, row, c); }
static const char *r_get(const void *n, int row, int c) { return PQgetvalue(n, row, c); }
static void r_free(void *n) { PQclear(n); }

static kn_db_type r_coltype(const void *n, int c) {
    switch (PQftype(n, c)) {
    case OID_BOOL: return KN_DB_T_BOOL;
    case OID_INT2: case OID_INT4: case OID_OID: case OID_INT8: return KN_DB_T_INT;
    case OID_FLOAT4: case OID_FLOAT8: case OID_NUMERIC: return KN_DB_T_FLOAT;
    case OID_JSON: case OID_JSONB: return KN_DB_T_JSON;
    case OID_BYTEA: return KN_DB_T_BYTES;
    default: return KN_DB_T_TEXT;
    }
}

static const kn_store_result_ops PG_RESULT = {r_rows, r_cols, r_colname, r_colnum, r_coltype, r_isnull, r_get, r_free};

/* Columns aliased with a type suffix (x__j, x__b ... knsvc/store.h) are only used
 * by portable .any texts: copy those results so names and types match the other drivers. */
static int has_suffix_alias(const PGresult *pg) {
    for (int c = 0; c < PQnfields(pg); c++) {
        const char *n = PQfname(pg, c);
        size_t l = strlen(n);
        if (l >= 4 && n[l - 3] == '_' && n[l - 2] == '_' && strchr("jbift", n[l - 1])) return 1;
    }
    return 0;
}

static kn_db_result *pg_materialize(PGresult *pg, int64_t affected) {
    int nc = PQnfields(pg);
    char **nb = calloc((size_t)(nc ? nc : 1), sizeof *nb);
    const char **names = calloc((size_t)(nc ? nc : 1), sizeof *names);
    kn_db_type *types = calloc((size_t)(nc ? nc : 1), sizeof *types);
    kn_db_result *r = NULL;
    if (nb && names && types) {
        for (int c = 0; c < nc; c++) {
            nb[c] = strdup(PQfname(pg, c));
            int t = nb[c] ? kn_store_alias_type(nb[c]) : -1;
            types[c] = t >= 0 ? (kn_db_type)t : r_coltype(pg, c);
            names[c] = nb[c];
        }
        r = kn_db_result_new(nc, names, types);
    }
    const char **cells = calloc((size_t)(nc ? nc : 1), sizeof *cells);
    for (int row = 0; r && cells && row < PQntuples(pg); row++) {
        for (int c = 0; c < nc; c++) cells[c] = PQgetisnull(pg, row, c) ? NULL : PQgetvalue(pg, row, c);
        if (kn_db_result_add_row(r, cells) != 0) break;
    }
    if (r) kn_db_result_set_affected(r, affected);
    for (int c = 0; nb && c < nc; c++) free(nb[c]);
    free(nb);
    free(names);
    free(types);
    free(cells);
    PQclear(pg);
    return r ? r : kn_db_result_error("53200", "out of memory");
}

/* Wrap a PGresult (takes ownership; may be NULL with conn for the error text). */
static kn_db_result *pg_result(PGresult *pg, PGconn *conn) {
    ExecStatusType st = pg ? PQresultStatus(pg) : PGRES_FATAL_ERROR;
    if (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK || st == PGRES_EMPTY_QUERY) {
        const char *s = PQcmdTuples(pg);
        int64_t affected = s && *s ? strtoll(s, NULL, 10) : 0;
        if (st == PGRES_TUPLES_OK && has_suffix_alias(pg)) return pg_materialize(pg, affected);
        kn_db_result *r = kn_db_result_native(&PG_RESULT, pg, affected);
        return r ? r : kn_db_result_error("53200", "out of memory");
    }
    const char *state = pg ? PQresultErrorField(pg, PG_DIAG_SQLSTATE) : NULL;
    const char *msg = pg ? PQresultErrorMessage(pg) : (conn ? PQerrorMessage(conn) : "no result");
    kn_db_result *r = kn_db_result_error(state ? state : (pg ? "" : "08006"), msg && *msg ? msg : "query failed");
    if (r && state && !strcmp(state, "P0001"))
        kn_db_result_set_user_message(r, PQresultErrorField(pg, PG_DIAG_MESSAGE_PRIMARY));
    if (r && pg) kn_db__result_attach(r, &PG_RESULT, pg); /* kn_db_result_pg() on errors too */
    else PQclear(pg);
    return r;
}

void *kn_db_result_pg(const kn_db_result *res) { return kn_db_result_native_ptr(res, &PG_RESULT); }

/* ------------------------------------------------------------------ connection parameters */

/* libpq keyword/value arrays for cfg (static storage inside out struct). */
typedef struct pg_conninfo {
    const char *keys[12];
    const char *vals[12];
    char port[16], timeout[16], options[512];
} pg_conninfo;

/* "-c search_path=a,b -c statement_timeout=N" with libpq option escaping (backslash before spaces). */
static void build_options(const kn_db_config *cfg, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    if (cfg->search_path && *cfg->search_path) {
        o += (size_t)snprintf(out + o, cap - o, "-c search_path=");
        for (const char *p = cfg->search_path; *p && o + 3 < cap; p++) {
            if (*p == ' ' || *p == '\\') out[o++] = '\\';
            out[o++] = *p;
        }
        out[o] = 0;
    }
    if (cfg->statement_timeout_ms > 0 && o < cap)
        snprintf(out + o, cap - o, "%s-c statement_timeout=%d", o ? " " : "", cfg->statement_timeout_ms);
}

static void pg_conninfo_build(const kn_db_config *cfg, pg_conninfo *ci) {
    int n = 0;
    snprintf(ci->port, sizeof ci->port, "%d", cfg->port > 0 ? cfg->port : 5432);
    snprintf(ci->timeout, sizeof ci->timeout, "%d", cfg->connect_timeout_s > 0 ? cfg->connect_timeout_s : 5);
    build_options(cfg, ci->options, sizeof ci->options);
#define KV(k, v) do { if ((v) && *(v)) { ci->keys[n] = (k); ci->vals[n] = (v); n++; } } while (0)
    KV("host", cfg->host ? cfg->host : "127.0.0.1");
    KV("port", ci->port);
    KV("dbname", cfg->name);
    KV("user", cfg->user);
    KV("password", cfg->password);
    KV("connect_timeout", ci->timeout);
    KV("application_name", cfg->application_name ? cfg->application_name : "knsvc");
    KV("options", ci->options);
    KV("client_encoding", "UTF8");
#undef KV
    ci->keys[n] = NULL;
    ci->vals[n] = NULL;
}

/* ------------------------------------------------------------------ async pool */

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
    kn_store_tx_cb tx_cb;    /* Q_BEGIN */
    void *ud;
    pg_tx *tx;         /* Q_END: transaction being finished */
    struct query *next;
} query;

typedef struct qlist {
    query *head, *tail;
    size_t len;
} qlist;

typedef struct pgc {
    pg_pool *db;
    PGconn *pg;
    int fd;
    int state;
    int listener;
    query *cur;
    int preparing;        /* cur is waiting for its PQsendPrepare */
    PGresult *last;       /* last result of cur */
    pg_tx *tx;         /* pinned transaction */
    char **prepared;
    size_t nprepared;
    kn_timer_id retry;
    int backoff_ms;
    qlist own;            /* listener: LISTEN commands */
} pgc;

struct pg_tx {
    pg_pool *db;
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

struct pg_pool {
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

static void tx_free(pg_tx *tx);

/* Deliver res for q (takes ownership of res), free q. */
static void query_complete(pg_pool *db, query *q, kn_db_result *res) {
    if (q->kind == Q_BEGIN) {
        if (q->tx_cb) q->tx_cb(kn_db_ok(res) ? q->tx : NULL, res, q->ud);
        if (!kn_db_ok(res) && q->tx) tx_free(q->tx);
    } else if (q->cb) {
        q->cb(res, q->ud);
    }
    kn_db_result_free(res);
    query_free(q);
}

static void query_fail(pg_pool *db, query *q, const char *state, const char *msg) {
    if (q->kind == Q_END && q->tx) {
        pg_tx *tx = q->tx;
        q->tx = NULL;
        tx_free(tx);
    }
    query_complete(db, q, kn_db_result_error(state, msg));
}

/* ------------------------------------------------------------------ transactions */

static void tx_free(pg_tx *tx) {
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
static void tx_break(pg_tx *tx) {
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
    pg_pool *db = c->db;
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
        pg_tx *tx = c->tx;
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
    pg_conninfo ci;
    pg_conninfo_build(&c->db->cfg, &ci);
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

static const stmt *find_stmt(const pg_pool *db, const char *name) {
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
static void pool_kick(pg_pool *db) {
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
    pg_pool *db = c->db;
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
            query_complete(db, q, pg_result(last, c->pg));
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
    kn_db_result *res = pg_result(last, c->pg);
    if (q->kind == Q_BEGIN && kn_db_ok(res)) {
        pg_tx *tx = calloc(1, sizeof *tx);
        if (tx) {
            tx->db = db;
            tx->conn = c;
            c->tx = tx;
        }
        q->tx = tx;
        if (!tx) {
            kn_db_result_free(res);
            res = kn_db_result_error("53200", "out of memory");
        }
    }
    if (q->kind == Q_END && kn_db_ok(res) && strcmp(q->sql, "COMMIT") == 0 && last &&
        strcmp(PQcmdStatus(last), "ROLLBACK") == 0) {
        /* COMMIT of an aborted transaction: PostgreSQL rolled back */
        kn_db_result_free(res);
        res = kn_db_result_error("40000", "transaction rolled back: a statement in it failed");
    }
    if (q->kind == Q_END) {
        pg_tx *tx = q->tx;
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

static pg_pool *pg_pool_new(kn_loop *loop, const kn_db_config *cfg) {
    pg_pool *db = calloc(1, sizeof *db);
    if (!db) return NULL;
    db->loop = loop;
    db->cfg = *cfg;
    const char **fields[] = {&db->cfg.host, &db->cfg.name, &db->cfg.user, &db->cfg.password,
                             &db->cfg.search_path, &db->cfg.application_name};
    for (int i = 0; i < 6; i++) {
        db->strs[i] = dup_or_null(*fields[i]);
        *fields[i] = db->strs[i];
    }
    db->cfg.driver = NULL;
    db->cfg.path = NULL;
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

static void fail_list(pg_pool *db, qlist *q) {
    query *x;
    while ((x = q_pop(q)) != NULL) query_fail(db, x, "08003", "database pool closed");
}

static void conn_shutdown(pgc *c) {
    pg_pool *db = c->db;
    kn_loop_cancel_timer(db->loop, c->retry);
    c->retry = 0;
    query *cur = c->cur;
    c->cur = NULL;
    conn_close_pg(c);
    if (cur) query_fail(db, cur, "08003", "database pool closed");
    fail_list(db, &c->own);
}

static void pg_pool_free(pg_pool *db) {
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

static int pg_ready_count(const pg_pool *db) {
    int n = 0;
    for (int i = 0; i < db->nconns; i++) n += db->conns[i].state == C_IDLE || db->conns[i].state == C_BUSY;
    return n;
}

static size_t pg_queue_len(const pg_pool *db) { return db->queue.len; }

static int enqueue(pg_pool *db, query *q) {
    if (!q) return -1;
    q_push(&db->queue, q);
    pool_kick(db);
    return 0;
}

static int pg_query(pg_pool *db, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!db || !sql || db->freeing) return -1;
    return enqueue(db, query_new(Q_SQL, sql, nparams, params, cb, ud));
}

static int pg_prepare(pg_pool *db, const char *name, const char *sql) {
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

static int pg_exec_prepared(pg_pool *db, const char *name, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!db || !name || db->freeing) return -1;
    return enqueue(db, query_new(Q_PREPARED, name, nparams, params, cb, ud));
}

/* ------------------------------------------------------------------ public: transactions */

static int pg_begin(pg_pool *db, kn_store_tx_cb cb, void *ud) {
    if (!db || db->freeing) return -1;
    query *q = query_new(Q_BEGIN, NULL, 0, NULL, NULL, ud);
    if (!q) return -1;
    q->tx_cb = cb;
    return enqueue(db, q);
}

static void broken_cb(kn_loop *loop, void *ud) {
    query *q = ud;
    pg_pool *db = q->tx ? q->tx->db : NULL;
    pg_tx *tx = q->kind == Q_END ? q->tx : NULL;
    q->tx = NULL;
    if (tx) tx_free(tx);
    query_complete(db, q, kn_db_result_error("08006", "transaction connection lost"));
}

static int tx_enqueue(pg_tx *tx, query *q) {
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

static int pg_tx_query(pg_tx *tx, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud) {
    if (!tx || tx->ending || !sql) return -1;
    query *q = query_new(Q_SQL, sql, nparams, params, cb, ud);
    if (q && !tx->conn) q->kind = Q_SQL;
    if (q && !tx->conn) {
        q->tx = tx;
        return kn_loop_post(tx->db->loop, broken_cb, q) ? -1 : 0;
    }
    return tx_enqueue(tx, q);
}

static int pg_tx_exec_prepared(pg_tx *tx, const char *name, int nparams, const char *const *params, kn_db_cb cb,
                           void *ud) {
    if (!tx || tx->ending || !name) return -1;
    return tx_enqueue(tx, query_new(Q_PREPARED, name, nparams, params, cb, ud));
}

static int tx_end(pg_tx *tx, const char *sql, kn_db_cb cb, void *ud) {
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

static int pg_tx_commit(pg_tx *tx, kn_db_cb cb, void *ud) { return tx_end(tx, "COMMIT", cb, ud); }
static int pg_tx_rollback(pg_tx *tx, kn_db_cb cb, void *ud) { return tx_end(tx, "ROLLBACK", cb, ud); }

/* ------------------------------------------------------------------ public: LISTEN/NOTIFY */

static int pg_listen(pg_pool *db, const char *channel, kn_db_notify_cb cb, void *ud) {
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

static int pg_notify_send(pg_pool *db, const char *channel, const char *payload) {
    return pg_query(db, "SELECT pg_notify($1, $2)", KN_DB_ARGS(channel, payload ? payload : ""), NULL, NULL);
}

/* ------------------------------------------------------------------ async ops (void * adapters) */

static void *a_pool_new(kn_loop *loop, const kn_db_config *cfg) { return pg_pool_new(loop, cfg); }
static void a_pool_free(void *p) { pg_pool_free(p); }
static int a_ready(const void *p) { return pg_ready_count(p); }
static size_t a_queue_len(const void *p) { return pg_queue_len(p); }
static int a_query(void *p, const char *sql, int n, const char *const *params, kn_db_cb cb, void *ud) {
    return pg_query(p, sql, n, params, cb, ud);
}
static int a_prepare(void *p, const char *name, const char *sql) { return pg_prepare(p, name, sql); }
static int a_exec_prepared(void *p, const char *name, int n, const char *const *params, kn_db_cb cb, void *ud) {
    return pg_exec_prepared(p, name, n, params, cb, ud);
}
static int a_begin(void *p, kn_store_tx_cb cb, void *ud) { return pg_begin(p, cb, ud); }
static int a_tx_query(void *tx, const char *sql, int n, const char *const *params, kn_db_cb cb, void *ud) {
    return pg_tx_query(tx, sql, n, params, cb, ud);
}
static int a_tx_exec_prepared(void *tx, const char *name, int n, const char *const *params, kn_db_cb cb, void *ud) {
    return pg_tx_exec_prepared(tx, name, n, params, cb, ud);
}
static int a_tx_commit(void *tx, kn_db_cb cb, void *ud) { return pg_tx_commit(tx, cb, ud); }
static int a_tx_rollback(void *tx, kn_db_cb cb, void *ud) { return pg_tx_rollback(tx, cb, ud); }
static int a_listen(void *p, const char *channel, kn_db_notify_cb cb, void *ud) { return pg_listen(p, channel, cb, ud); }
static int a_notify(void *p, const char *channel, const char *payload) { return pg_notify_send(p, channel, payload); }

static const kn_store_async_ops PG_ASYNC = {
    a_pool_new, a_pool_free, a_ready, a_queue_len, a_query, a_prepare, a_exec_prepared,
    a_begin, a_tx_query, a_tx_exec_prepared, a_tx_commit, a_tx_rollback, a_listen, a_notify};

/* ------------------------------------------------------------------ blocking ops */

static void *b_connect(const kn_db_config *cfg, char *err, size_t errlen) {
    pg_conninfo ci;
    pg_conninfo_build(cfg, &ci);
    PGconn *pg = PQconnectdbParams(ci.keys, ci.vals, 0);
    if (!pg || PQstatus(pg) != CONNECTION_OK) {
        if (err && errlen) {
            snprintf(err, errlen, "%s", pg ? PQerrorMessage(pg) : "out of memory");
            size_t n = strlen(err);
            while (n && err[n - 1] == '\n') err[--n] = 0;
        }
        PQfinish(pg);
        return NULL;
    }
    PQsetNoticeReceiver(pg, quiet_notice, NULL);
    return pg;
}

static void b_disconnect(void *c) { PQfinish(c); }

static kn_db_result *b_exec(void *c, const char *sql, int n, const char *const *params) {
    return pg_result(PQexecParams(c, sql, n, NULL, params, NULL, NULL, 0), c);
}

static int b_exec_script(void *c, const char *sql, char *err, size_t errlen) {
    kn_db_result *res = pg_result(PQexec(c, sql), c);
    int ok = kn_db_ok(res);
    if (!ok && err && errlen) snprintf(err, errlen, "%s", kn_db_error(res));
    kn_db_result_free(res);
    return ok ? 0 : -1;
}

static int b_broken(void *c) { return PQstatus(c) == CONNECTION_BAD; }

/* Session advisory lock; "kn_migrate" keeps the historic constant key. */
static int b_lock(void *c, const char *name, int on) {
    char key[32];
    if (!strcmp(name, "kn_migrate")) {
        snprintf(key, sizeof key, "7020896536452359");
    } else {
        uint64_t h = 1469598103934665603ULL;
        for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ULL;
        snprintf(key, sizeof key, "%lld", (long long)(h >> 1));
    }
    kn_db_result *r = b_exec(c, on ? "SELECT pg_advisory_lock($1::bigint)" : "SELECT pg_advisory_unlock($1::bigint)",
                             KN_DB_ARGS(key));
    int ok = kn_db_ok(r);
    kn_db_result_free(r);
    return ok ? 0 : -1;
}

static void b_interrupt(void *c) {
    PGcancel *cn = PQgetCancel(c);
    if (!cn) return;
    char err[128];
    PQcancel(cn, err, sizeof err);
    PQfreeCancel(cn);
}

static void *b_native(void *c) { return c; }

static const kn_store_blocking_ops PG_BLOCKING = {
    b_connect, b_disconnect, b_exec, b_exec_script, b_broken, b_lock, b_interrupt, b_native, "BEGIN"};

void *kn_dbconn_pg(kn_dbconn *c) {
    return c && c->drv && !strcmp(c->drv->name, "postgres") ? c->conn : NULL;
}

/* {macros} of portable kn_sql texts (.any), PostgreSQL flavour */
static const kn_sql_macro PG_MACROS[] = {
    {"now", "now()"},
    {"ago", "(now() - make_interval(secs => (%1)::double precision))"},
    {"later", "(now() + make_interval(secs => (%1)::double precision))"},
    {"epoch_ms", "(extract(epoch FROM %1) * 1000)::bigint"},
    {"from_ms", "to_timestamp((%1)::double precision / 1000)"},
    {"int", "(%1)::bigint"},
    {"float", "(%1)::double precision"},
    {"text", "(%1)::text"},
    {"bool", "(%1)::boolean"},
    {"json", "(%1)::jsonb"},
    {"jagg", "jsonb_agg(%*)"},
    {"html", "replace(replace(replace(replace(replace(coalesce((%1)::text, ''), '&', '&amp;'), '<', '&lt;'),"
             " '>', '&gt;'), '\"', '&quot;'), '''', '&#39;')"},
    {"hash", "sha256(convert_to(%1, 'UTF8'))"},
    {"greatest", "greatest(%*)"},
    {"least", "least(%*)"},
    {"insert_ignore", "INSERT"},
    {"ignore_conflicts", "ON CONFLICT DO NOTHING"},
    {"for_update", "FOR UPDATE"},
    {"serial_pk", "integer GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY"},
    {"bigserial_pk", "bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY"},
    {"ilike", "ILIKE"},
    {"true", "true"},
    {"false", "false"},
    {NULL, NULL}};

const kn_store_driver kn_store_driver_postgres = {
    .name = "postgres",
    .dialect = "postgres",
    .caps = KN_DB_CAP_NOTIFY | KN_DB_CAP_ASYNC | KN_DB_CAP_RETURNING | KN_DB_CAP_PROCS_SQL | KN_DB_CAP_TX_DDL,
    .default_port = 5432,
    .default_pool = 4,
    .macros = PG_MACROS,
    .blocking = &PG_BLOCKING,
    .async = &PG_ASYNC,
};
