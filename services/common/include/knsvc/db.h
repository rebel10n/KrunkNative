/*
 * knsvc/db.h - database access for services: an async connection pool on a
 * kn_loop, transactions, prepared statements, typed results, publish/subscribe
 * and a small synchronous connection for CLI tools (migrations, tests).
 *
 * The backend is a pluggable driver chosen by config `database.driver`
 * (postgres | sqlite | mysql, see knsvc/store.h for writing one). Part of the
 * static library `knsvc_db`; the core `knsvc` library has no database code so
 * game servers can link it.
 *
 * Parameterized queries only: SQL text uses $1..$n and values are passed as
 * text in params[] (NULL element = SQL NULL). Never format values into SQL.
 *     kn_db_query(db, "SELECT id, kr FROM accounts WHERE name = $1",
 *                 KN_DB_ARGS(name), on_account, ctx);
 *
 * kn_db_query() sends the text to whatever database is configured. Code that
 * must run on every driver uses kn_sql (one text per dialect, or a portable
 * text with {macros}) and kn_proc (multi-statement operations: server-side SQL
 * on PostgreSQL, C on the other drivers):
 *     static const kn_sql Q = {.pg = "... now() ...", .any = "... {now} ..."};
 *     kn_db_query_sql(db, &Q, KN_DB_ARGS(a), cb, ud);
 *
 * Queries run concurrently on different pool connections: there is no ordering
 * between two kn_db_query calls. Chain dependent queries in callbacks or use a
 * transaction (one connection, statements run in order).
 *
 * Every async call delivers exactly one callback with a kn_db_result (never
 * NULL); check kn_db_ok(). The result is freed after the callback returns.
 * Queries queue when every connection is busy; connections reconnect with
 * backoff, and queries that were waiting are run once a connection is back
 * (queries in flight on a dropped connection fail with sqlstate "08006").
 * kn_db_free() fails every queued query with "08003" (callbacks run).
 * Errors carry a PostgreSQL-style SQLSTATE on every driver (kn_db_errclass).
 */
#ifndef KNSVC_DB_H
#define KNSVC_DB_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "knsvc/loop.h"

struct kn_config;

typedef struct kn_db kn_db;
typedef struct kn_db_tx kn_db_tx;
typedef struct kn_db_result kn_db_result;
typedef struct kn_dbconn kn_dbconn;

typedef struct kn_db_config {
    const char *host;             /* default "127.0.0.1" */
    int port;                     /* default: the driver's (5432, 3306) */
    const char *name;             /* database */
    const char *user;
    const char *password;         /* never logged */
    const char *search_path;      /* postgres: SET search_path on connect (schema isolation) */
    const char *application_name; /* default "knsvc" */
    int pool_size;                /* default: the driver's (4; sqlite 2) */
    int connect_timeout_s;        /* default 5 */
    int statement_timeout_ms;     /* 0 = server default */
    /* ---- driver selection (knsvc/store.h) */
    const char *driver;           /* "postgres" | "sqlite" | "mysql" | NULL = default (see kn_db_config_from) */
    const char *path;             /* sqlite: database file (default data/coordinator.db) */
    int notify_poll_ms;           /* drivers without native pub/sub: poll interval (default 100) */
    int busy_timeout_ms;          /* sqlite: wait for the write lock (default 5000) */
} kn_db_config;

/* Fill from config section "database" (driver, host, port, name, user, password,
 * search_path, path, pool_size, connect_timeout_s, statement_timeout_ms,
 * notify_poll_ms, busy_timeout_ms) incl. the KN_DATABASE_* env / CLI overrides.
 * Without database.driver: "postgres" when database.host / name / user is set
 * (and the postgres driver is built in), else "sqlite". Strings point into cfg
 * (path: resolved against the config root into static storage). */
void kn_db_config_from(const struct kn_config *cfg, kn_db_config *out);

typedef void (*kn_db_cb)(kn_db_result *res, void *ud);
typedef void (*kn_db_tx_cb)(kn_db_tx *tx, kn_db_result *res, void *ud); /* tx NULL on failure */
typedef void (*kn_db_notify_cb)(const char *channel, const char *payload, void *ud);

/* Count + array literal for params: KN_DB_ARGS(a, b) -> 2, (const char *const[]){a, b} */
#define KN_DB_ARGS(...) \
    (int)(sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *)), (const char *const[]){__VA_ARGS__}

/* ---- dialect-aware SQL. The first non-NULL of (the driver's dialect text,
 * .any) is used; .pg is sent verbatim, .sqlite / .mysql / .any (also .any on
 * postgres) expand {macros} outside string literals (table in
 * doc/backend.md 3.6): {now} {ago:s} {later:s} {epoch_ms:ts} {from_ms:ms}
 * {int:x} {float:x} {text:x} {bool:x} {json:x} {jagg:x} {html:x} {hash:x}
 * {greatest:a,b} {least:a,b} {insert_ignore} ... {ignore_conflicts}
 * {for_update} {serial_pk} {bigserial_pk} {ilike} {true} {false}.
 * Column aliases ending in __j __b __i __f __t force the result type (json,
 * bool, int, float, text) on every driver; the suffix is stripped. */
typedef struct kn_sql {
    const char *pg;
    const char *sqlite;
    const char *mysql;
    const char *any;
} kn_sql;

/* ---- multi-statement operations. On a driver with server-side functions
 * (postgres) and .pg set, the .pg SQL text is run like kn_db_query. Otherwise
 * run() executes in C on a pool connection inside a transaction (BEGIN ..
 * COMMIT, rolled back when it returns an error): it runs on a worker thread,
 * so it may only touch its arguments and read-only data. It returns a result
 * shaped like the .pg query's (build it with knsvc/store.h helpers), or an
 * error (kn_db_result_user_error for user-facing refusals). */
typedef kn_db_result *(*kn_proc_fn)(kn_dbconn *c, int nparams, const char *const *params);
typedef struct kn_proc {
    const char *name;   /* for logs */
    const char *pg;     /* postgres text (usually "SELECT fn($1, ...)"), or NULL */
    kn_proc_fn run;     /* C implementation for every other driver (and postgres when .pg is NULL) */
} kn_proc;

/* ---- pool */
kn_db *kn_db_new(kn_loop *loop, const kn_db_config *cfg); /* copies cfg; starts connecting. NULL: unknown driver */
void kn_db_free(kn_db *db);
int kn_db_ready_count(const kn_db *db);   /* connected connections */
size_t kn_db_queue_len(const kn_db *db);  /* waiting queries */
/* Block (running the loop) until at least one connection is up or timeout. 0 ok, -1 timeout. */
int kn_db_wait_ready(kn_db *db, int timeout_ms);
const char *kn_db_driver_name(const kn_db *db); /* "postgres" ... */
const char *kn_db_dialect(const kn_db *db);     /* kn_sql variant used */
unsigned kn_db_caps(const kn_db *db);           /* KN_DB_CAP_* (knsvc/store.h) */

int kn_db_query(kn_db *db, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud);
int kn_db_query_sql(kn_db *db, const kn_sql *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud);
int kn_db_call(kn_db *db, const kn_proc *proc, int nparams, const char *const *params, kn_db_cb cb, void *ud);

/* Prepared statements: register once (any time), executed by name. Each pool
 * connection prepares it lazily on first use (and again after a reconnect). */
int kn_db_prepare(kn_db *db, const char *name, const char *sql);
int kn_db_exec_prepared(kn_db *db, const char *name, int nparams, const char *const *params, kn_db_cb cb, void *ud);

/* ---- transactions: a pinned connection from BEGIN until commit/rollback. */
int kn_db_begin(kn_db *db, kn_db_tx_cb cb, void *ud);
int kn_db_tx_query(kn_db_tx *tx, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud);
int kn_db_tx_query_sql(kn_db_tx *tx, const kn_sql *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud);
/* kn_proc inside the transaction (no nested BEGIN; a failure aborts the transaction). */
int kn_db_tx_call(kn_db_tx *tx, const kn_proc *proc, int nparams, const char *const *params, kn_db_cb cb, void *ud);
int kn_db_tx_exec_prepared(kn_db_tx *tx, const char *name, int nparams, const char *const *params, kn_db_cb cb, void *ud);
/* End the transaction; cb (may be NULL) gets the COMMIT/ROLLBACK result. tx is invalid afterwards.
 * A commit after a failed statement reports the error and rolls back (every driver). */
int kn_db_tx_commit(kn_db_tx *tx, kn_db_cb cb, void *ud);
int kn_db_tx_rollback(kn_db_tx *tx, kn_db_cb cb, void *ud);

/* ---- publish/subscribe. postgres: LISTEN/NOTIFY on a dedicated connection
 * (re-LISTENs after reconnects). Other drivers: rows in table kn_pubsub,
 * polled every database.notify_poll_ms (latency up to that interval; rows are
 * pruned after a minute). Payloads up to 7900 bytes. */
int kn_db_listen(kn_db *db, const char *channel, kn_db_notify_cb cb, void *ud);
int kn_db_notify(kn_db *db, const char *channel, const char *payload); /* fire and forget */

/* External pub/sub transport (the brain's optional Redis hot tier, doc/backend.md 3.11).
 * Once set, kn_db_notify() publishes through ops->publish when that returns 0 (else
 * through the database as above), and every kn_db_listen() also subscribes through
 * ops->subscribe. The database path keeps listening (postgres LISTEN; the kn_pubsub
 * poll every slow_poll_ms instead of notify_poll_ms while the transport reports itself
 * up with kn_db_pubsub_transport_up), so a writer whose transport is down still
 * reaches every listener. Call before the first kn_db_listen; -1 afterwards. */
typedef struct kn_db_pubsub_ops {
    int (*publish)(void *ctx, const char *channel, const char *payload);  /* 0 = sent */
    int (*subscribe)(void *ctx, const char *channel, kn_db_notify_cb cb, void *ud);
} kn_db_pubsub_ops;
int kn_db_set_pubsub(kn_db *db, const kn_db_pubsub_ops *ops, void *ctx, int slow_poll_ms);
void kn_db_pubsub_transport_up(kn_db *db, int up);

/* ---- results */
typedef enum kn_db_type {
    KN_DB_T_TEXT = 0,
    KN_DB_T_INT,
    KN_DB_T_FLOAT,
    KN_DB_T_BOOL,
    KN_DB_T_JSON,
    KN_DB_T_BYTES
} kn_db_type;

/* Error classes (driver-native codes are mapped to SQLSTATEs). */
typedef enum kn_db_errclass {
    KN_DB_E_NONE = 0,       /* ok */
    KN_DB_E_UNIQUE,         /* 23505 unique / primary key violation */
    KN_DB_E_CONSTRAINT,     /* other 23xxx: foreign key, check, not null */
    KN_DB_E_USER,           /* P0001: refusal raised by our SQL/C procedures; kn_db_user_message() */
    KN_DB_E_CONNECTION,     /* 08xxx: connection lost / pool closed */
    KN_DB_E_RETRY,          /* 40001 / 40P01 / 55P03: serialization, deadlock, lock busy */
    KN_DB_E_TX_ABORTED,     /* 25P02 / 40000: statement in / commit of a failed transaction */
    KN_DB_E_TIMEOUT,        /* 57014 statement timeout / cancelled */
    KN_DB_E_SYNTAX,         /* 42xxx: syntax error, undefined table/column/function */
    KN_DB_E_OTHER
} kn_db_errclass;

int kn_db_ok(const kn_db_result *res);                 /* 1 = command/tuples ok */
const char *kn_db_error(const kn_db_result *res);      /* "" when ok */
const char *kn_db_sqlstate(const kn_db_result *res);   /* "23505" etc., "" when ok/unknown */
kn_db_errclass kn_db_errclass_of(const kn_db_result *res);
int kn_db_is_unique_violation(const kn_db_result *res);/* sqlstate 23505 */
/* User-facing message of a P0001 refusal (RAISE ... MESSAGE / kn_db_result_user_error), else NULL. */
const char *kn_db_user_message(const kn_db_result *res);
int kn_db_rows(const kn_db_result *res);
int kn_db_cols(const kn_db_result *res);
int64_t kn_db_affected(const kn_db_result *res);       /* rows affected by INSERT/UPDATE/DELETE */
int64_t kn_db_last_id(const kn_db_result *res);        /* auto-increment id of an INSERT (sqlite/mysql), else 0 */
int kn_db_col(const kn_db_result *res, const char *name); /* -1 if absent */
const char *kn_db_colname(const kn_db_result *res, int col);
kn_db_type kn_db_coltype(const kn_db_result *res, int col);
int kn_db_isnull(const kn_db_result *res, int row, int col);
const char *kn_db_get(const kn_db_result *res, int row, int col);  /* text, "" for NULL */
const char *kn_db_get_by(const kn_db_result *res, int row, const char *col); /* NULL if col absent */
int64_t kn_db_get_int64(const kn_db_result *res, int row, int col, int64_t def);
double kn_db_get_double(const kn_db_result *res, int row, int col, double def);
int kn_db_get_bool(const kn_db_result *res, int row, int col, int def);   /* 't'/'f', 1/0, true/false */
/* Rows as a JSON array of objects; int/float/bool/json columns typed, NULL -> null. */
cJSON *kn_db_result_to_json(const kn_db_result *res);
cJSON *kn_db_row_to_json(const kn_db_result *res, int row);
void *kn_db_result_pg(const kn_db_result *res);        /* postgres: underlying PGresult* (else NULL) */
void kn_db_result_free(kn_db_result *res);             /* only for kn_dbconn_exec results */

/* ---- synchronous single connection (tools, tests, migrations, kn_proc). Blocking. */
kn_dbconn *kn_dbconn_open(const kn_db_config *cfg, char *err, size_t errlen);
void kn_dbconn_close(kn_dbconn *c);
kn_db_result *kn_dbconn_exec(kn_dbconn *c, const char *sql, int nparams, const char *const *params); /* free it */
kn_db_result *kn_dbconn_exec_sql(kn_dbconn *c, const kn_sql *sql, int nparams, const char *const *params);
/* Run a kn_proc on this connection (no transaction of its own). */
kn_db_result *kn_dbconn_call(kn_dbconn *c, const kn_proc *proc, int nparams, const char *const *params);
/* Multi-statement script without params (simple query protocol). 0 ok, -1 error (msg in err). */
int kn_dbconn_exec_script(kn_dbconn *c, const char *sql, char *err, size_t errlen);
const char *kn_dbconn_driver_name(const kn_dbconn *c);
const char *kn_dbconn_dialect(const kn_dbconn *c);
void *kn_dbconn_native(kn_dbconn *c);                  /* PGconn* / sqlite3* / MYSQL* */
void *kn_dbconn_pg(kn_dbconn *c);                      /* PGconn* (NULL on other drivers) */

#endif
