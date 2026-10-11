/*
 * knsvc/db.h - PostgreSQL over libpq: async connection pool on a kn_loop,
 * transactions, lazily prepared statements, LISTEN/NOTIFY, and a small
 * synchronous connection for CLI tools (migrations, tests).
 *
 * Part of the separate static library `knsvc_db` (links libpq); the core
 * `knsvc` library stays libpq-free so game servers can link it.
 *
 * Parameterized queries only: SQL text uses $1..$n and values are passed as
 * text in params[] (NULL element = SQL NULL). Never format values into SQL.
 *     kn_db_query(db, "SELECT id, kr FROM accounts WHERE name = $1",
 *                 KN_DB_ARGS(name), on_account, ctx);
 *     kn_db_query(db, "SELECT now()", 0, NULL, cb, ud);
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
    int port;                     /* default 5432 */
    const char *name;             /* database */
    const char *user;
    const char *password;         /* never logged */
    const char *search_path;      /* optional: SET search_path on connect (schema isolation) */
    const char *application_name; /* default "knsvc" */
    int pool_size;                /* default 4 */
    int connect_timeout_s;        /* default 5 */
    int statement_timeout_ms;     /* 0 = server default */
} kn_db_config;

/* Fill from config section "database" (host, port, name, user, password,
 * search_path, pool_size, connect_timeout_s, statement_timeout_ms) incl. the
 * KN_DATABASE_* env / CLI overrides. Strings point into cfg. */
void kn_db_config_from(const struct kn_config *cfg, kn_db_config *out);

typedef void (*kn_db_cb)(kn_db_result *res, void *ud);
typedef void (*kn_db_tx_cb)(kn_db_tx *tx, kn_db_result *res, void *ud); /* tx NULL on failure */
typedef void (*kn_db_notify_cb)(const char *channel, const char *payload, void *ud);

/* Count + array literal for params: KN_DB_ARGS(a, b) -> 2, (const char *const[]){a, b} */
#define KN_DB_ARGS(...) \
    (int)(sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *)), (const char *const[]){__VA_ARGS__}

/* ---- pool */
kn_db *kn_db_new(kn_loop *loop, const kn_db_config *cfg); /* copies cfg; starts connecting */
void kn_db_free(kn_db *db);
int kn_db_ready_count(const kn_db *db);   /* connected connections */
size_t kn_db_queue_len(const kn_db *db);  /* waiting queries */
/* Block (running the loop) until at least one connection is up or timeout. 0 ok, -1 timeout. */
int kn_db_wait_ready(kn_db *db, int timeout_ms);

int kn_db_query(kn_db *db, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud);

/* Prepared statements: register once (any time), executed by name. Each pool
 * connection prepares it lazily on first use (and again after a reconnect). */
int kn_db_prepare(kn_db *db, const char *name, const char *sql);
int kn_db_exec_prepared(kn_db *db, const char *name, int nparams, const char *const *params, kn_db_cb cb, void *ud);

/* ---- transactions: a pinned connection from BEGIN until commit/rollback. */
int kn_db_begin(kn_db *db, kn_db_tx_cb cb, void *ud);
int kn_db_tx_query(kn_db_tx *tx, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud);
int kn_db_tx_exec_prepared(kn_db_tx *tx, const char *name, int nparams, const char *const *params, kn_db_cb cb, void *ud);
/* End the transaction; cb (may be NULL) gets the COMMIT/ROLLBACK result. tx is invalid afterwards.
 * A commit after a failed statement reports the error (PostgreSQL rolls back). */
int kn_db_tx_commit(kn_db_tx *tx, kn_db_cb cb, void *ud);
int kn_db_tx_rollback(kn_db_tx *tx, kn_db_cb cb, void *ud);

/* ---- LISTEN/NOTIFY on a dedicated connection (re-LISTENs after reconnects). */
int kn_db_listen(kn_db *db, const char *channel, kn_db_notify_cb cb, void *ud);
int kn_db_notify(kn_db *db, const char *channel, const char *payload); /* pg_notify($1,$2), fire and forget */

/* ---- results */
int kn_db_ok(const kn_db_result *res);                 /* 1 = command/tuples ok */
const char *kn_db_error(const kn_db_result *res);      /* "" when ok */
const char *kn_db_sqlstate(const kn_db_result *res);   /* "23505" etc., "" when ok/unknown */
int kn_db_is_unique_violation(const kn_db_result *res);/* sqlstate 23505 */
int kn_db_rows(const kn_db_result *res);
int kn_db_cols(const kn_db_result *res);
int64_t kn_db_affected(const kn_db_result *res);       /* rows affected by INSERT/UPDATE/DELETE */
int kn_db_col(const kn_db_result *res, const char *name); /* -1 if absent */
const char *kn_db_colname(const kn_db_result *res, int col);
int kn_db_isnull(const kn_db_result *res, int row, int col);
const char *kn_db_get(const kn_db_result *res, int row, int col);  /* text, "" for NULL */
const char *kn_db_get_by(const kn_db_result *res, int row, const char *col); /* NULL if col absent */
int64_t kn_db_get_int64(const kn_db_result *res, int row, int col, int64_t def);
double kn_db_get_double(const kn_db_result *res, int row, int col, double def);
int kn_db_get_bool(const kn_db_result *res, int row, int col, int def);   /* 't'/'f' */
/* Rows as a JSON array of objects; int/float/bool/json/jsonb columns typed, NULL -> null. */
cJSON *kn_db_result_to_json(const kn_db_result *res);
cJSON *kn_db_row_to_json(const kn_db_result *res, int row);
void *kn_db_result_pg(const kn_db_result *res);        /* underlying PGresult* (may be NULL) */
void kn_db_result_free(kn_db_result *res);             /* only for kn_dbconn_exec results */

/* ---- synchronous single connection (tools, tests, migrations). Blocking. */
kn_dbconn *kn_dbconn_open(const kn_db_config *cfg, char *err, size_t errlen);
void kn_dbconn_close(kn_dbconn *c);
kn_db_result *kn_dbconn_exec(kn_dbconn *c, const char *sql, int nparams, const char *const *params); /* free it */
/* Multi-statement script without params (simple query protocol). 0 ok, -1 error (msg in err). */
int kn_dbconn_exec_script(kn_dbconn *c, const char *sql, char *err, size_t errlen);
void *kn_dbconn_pg(kn_dbconn *c);                      /* PGconn* */

#endif
