/*
 * knsvc/store.h - the database adapter (driver) interface behind knsvc/db.h,
 * and the result builders used by drivers and kn_proc implementations.
 *
 * Applications use knsvc/db.h (kn_db_*). A driver is one .c file defining
 * `const kn_store_driver kn_store_driver_<name>` plus one
 * knsvc_store_driver(<OPTION> <name> <source> <libs>) line in
 * services/common/CMakeLists.txt (built when option KNSVC_STORE_<OPTION> is ON;
 * the generated driver list is included by src/store_drivers.c).
 * doc/backend.md 3.6 "Writing a new adapter".
 *
 * Two driver shapes:
 *   - blocking drivers (sqlite, mysql): implement kn_store_blocking_ops only.
 *     The core runs them on a pool of worker threads (src/store_worker.c), one
 *     connection per thread, so the kn_loop never blocks; it provides queueing,
 *     reconnects, transactions, prepared statement names, kn_proc and pub/sub
 *     emulation (table kn_pubsub) on top.
 *   - async drivers (postgres): additionally implement kn_store_async_ops on the
 *     loop themselves. kn_dbconn (tools, migrations) always uses `blocking`, and
 *     kn_proc implementations without a .pg text run on a small worker pool of
 *     blocking connections.
 *
 * SQL reaching a driver is already resolved for its dialect and {macro}-expanded
 * (src/store_sql.c). Placeholders are $1..$n; a driver that needs another style
 * rewrites them (kn_store_sql_numbered / kn_store_sql_qmark). Parameters are
 * text or NULL; kn_store_param_int() tells canonical integers apart.
 *
 * Results: wrap a native result (kn_db_result_native + kn_store_result_ops) or
 * build a materialized one (kn_db_result_new / kn_db_result_add_row). Errors
 * carry a PostgreSQL SQLSTATE: map native codes (23505 unique, 23503 foreign
 * key, 23514 check, 23502 not null, 08006 connection lost, 40001 busy, 42601
 * syntax, 42P01 undefined table, 57014 timeout, P0001 user refusal).
 */
#ifndef KNSVC_STORE_H
#define KNSVC_STORE_H

#include "knsvc/db.h"

/* ---- capabilities (kn_db_caps) */
enum {
    KN_DB_CAP_NOTIFY = 1u << 0,    /* native publish/subscribe (else the core polls kn_pubsub rows) */
    KN_DB_CAP_ASYNC = 1u << 1,     /* async ops on the loop (else worker threads) */
    KN_DB_CAP_RETURNING = 1u << 2, /* INSERT/UPDATE/DELETE ... RETURNING */
    KN_DB_CAP_PROCS_SQL = 1u << 3, /* kn_proc.pg texts run (server-side functions from the migrations) */
    KN_DB_CAP_TX_DDL = 1u << 4     /* DDL is transactional (migrations are atomic) */
};

/* ---- {macro} table of a dialect: {name} or {name:a,b} -> expansion with %1..%9
 * replaced by the arguments (top-level commas split them) and %* by all of them. */
typedef struct kn_sql_macro {
    const char *name;
    const char *expansion;
} kn_sql_macro;

/* ---- native results */
typedef struct kn_store_result_ops {
    int (*rows)(const void *native);
    int (*cols)(const void *native);
    const char *(*colname)(const void *native, int col);
    int (*colnum)(const void *native, const char *name);   /* -1 if absent */
    kn_db_type (*coltype)(const void *native, int col);
    int (*isnull)(const void *native, int row, int col);
    const char *(*get)(const void *native, int row, int col);
    void (*free)(void *native);
} kn_store_result_ops;

/* ---- result builders (drivers, kn_proc implementations) */
kn_db_result *kn_db_result_error(const char *sqlstate, const char *msg);
/* P0001 refusal whose message is shown to users (kn_db_user_message). */
kn_db_result *kn_db_result_user_error(const char *msg);
/* Ok result without rows (affected rows / last id may be set later). */
kn_db_result *kn_db_result_command(int64_t affected);
/* Materialized result: names/types copied (types may be NULL = text). */
kn_db_result *kn_db_result_new(int ncols, const char *const *names, const kn_db_type *types);
/* Append a row of ncols cells (NULL = SQL NULL); copies. 0 ok, -1 oom. */
int kn_db_result_add_row(kn_db_result *res, const char *const *cells);
int kn_db_result_add_row_n(kn_db_result *res, const char *const *cells, const size_t *lens);
/* One row, one column `name` holding JSON (consumes v; NULL v -> SQL NULL). */
kn_db_result *kn_db_result_json(const char *name, cJSON *v);
/* One row, one column, text or int64. */
kn_db_result *kn_db_result_text(const char *name, kn_db_type type, const char *value);
kn_db_result *kn_db_result_int(const char *name, int64_t v);
void kn_db_result_set_type(kn_db_result *res, int col, kn_db_type type);
kn_db_result *kn_db_result_native(const kn_store_result_ops *ops, void *native, int64_t affected);
void *kn_db_result_native_ptr(const kn_db_result *res, const kn_store_result_ops *ops);
void kn_db_result_set_user_message(kn_db_result *res, const char *msg);
void kn_db_result_set_affected(kn_db_result *res, int64_t affected);
void kn_db_result_set_last_id(kn_db_result *res, int64_t id);

/* ---- blocking connection ops (kn_dbconn, worker pool) */
typedef struct kn_store_blocking_ops {
    /* Open a connection; NULL with a message in err. */
    void *(*connect)(const kn_db_config *cfg, char *err, size_t errlen);
    void (*disconnect)(void *conn);
    /* One parameterized statement -> result (never NULL). */
    kn_db_result *(*exec)(void *conn, const char *sql, int nparams, const char *const *params);
    /* Multi-statement script without parameters. 0 ok, -1 error (message in err). */
    int (*exec_script)(void *conn, const char *sql, char *err, size_t errlen);
    /* 1 when the connection is unusable (after a connection-class error). */
    int (*broken)(void *conn);
    /* Session-level named lock for migrations (NULL: transactions serialize). 0 ok. */
    int (*lock)(void *conn, const char *name, int on);
    /* Abort the statement running on conn from another thread (may be NULL). */
    void (*interrupt)(void *conn);
    void *(*native)(void *conn);  /* kn_dbconn_native() */
    const char *begin_sql;        /* "BEGIN", "BEGIN IMMEDIATE", "START TRANSACTION" */
} kn_store_blocking_ops;

/* ---- async pool ops (drivers integrated with the loop). Transaction handles
 * are the driver's own (void *); the core wraps them in kn_db_tx. */
typedef void (*kn_store_tx_cb)(void *tx, kn_db_result *res, void *ud); /* tx NULL on failure */
typedef struct kn_store_async_ops {
    void *(*pool_new)(kn_loop *loop, const kn_db_config *cfg);
    void (*pool_free)(void *pool);
    int (*ready_count)(const void *pool);
    size_t (*queue_len)(const void *pool);
    int (*query)(void *pool, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud);
    int (*prepare)(void *pool, const char *name, const char *sql);
    int (*exec_prepared)(void *pool, const char *name, int nparams, const char *const *params, kn_db_cb cb, void *ud);
    int (*begin)(void *pool, kn_store_tx_cb cb, void *ud);
    int (*tx_query)(void *tx, const char *sql, int nparams, const char *const *params, kn_db_cb cb, void *ud);
    int (*tx_exec_prepared)(void *tx, const char *name, int nparams, const char *const *params, kn_db_cb cb,
                            void *ud);
    int (*tx_commit)(void *tx, kn_db_cb cb, void *ud);
    int (*tx_rollback)(void *tx, kn_db_cb cb, void *ud);
    /* native pub/sub (KN_DB_CAP_NOTIFY) */
    int (*listen)(void *pool, const char *channel, kn_db_notify_cb cb, void *ud);
    int (*notify)(void *pool, const char *channel, const char *payload);
} kn_store_async_ops;

typedef struct kn_store_driver {
    const char *name;             /* database.driver value */
    const char *dialect;          /* kn_sql variant: "postgres" | "sqlite" | "mysql" (or a new one: uses .any) */
    unsigned caps;                /* KN_DB_CAP_* */
    int default_port;             /* 0: not a network database */
    int default_pool;             /* pool size when database.pool_size is absent */
    const kn_sql_macro *macros;   /* {name} expansions, {NULL, NULL}-terminated */
    const kn_store_blocking_ops *blocking; /* required */
    const kn_store_async_ops *async;       /* NULL: worker threads run `blocking` */
} kn_store_driver;

/* ---- registry (src/store_drivers.c lists the built-in drivers) */
const kn_store_driver *kn_store_driver_find(const char *name);
size_t kn_store_driver_count(void);
const kn_store_driver *kn_store_driver_at(size_t i);
/* Drivers linked in by the application itself (call before kn_db_new). 0 ok. */
int kn_store_driver_register(const kn_store_driver *drv);

/* ---- SQL helpers (src/store_sql.c) */
/* Text of sql for a dialect, macro-expanded (malloc'd; NULL when no variant fits). */
char *kn_sql_resolve(const kn_sql *sql, const kn_store_driver *drv);
/* Expand {macros} of text with a driver's table (malloc'd). */
char *kn_sql_expand(const char *text, const kn_sql_macro *macros);
/* $n -> ?n (sqlite numbered parameters), skipping quoted text. malloc'd. */
char *kn_store_sql_numbered(const char *sql);
/* $n -> ?: returns malloc'd SQL; *order (malloc'd) lists the 0-based parameter
 * index of every ?, *count their number; *limit_ctx[k] = 1 when the k-th ? follows
 * LIMIT/OFFSET (drivers that inline values must not quote those). */
char *kn_store_sql_qmark(const char *sql, int **order, unsigned char **limit_ctx, int *count);
/* Inline every $n as a literal: NULL, a bare integer after LIMIT/OFFSET, else
 * '<escape(value)>'. escape writes at most 2*len+1 bytes to `to` (no quotes) and
 * returns the length written. malloc'd. */
typedef size_t (*kn_store_escape_fn)(void *ud, char *to, const char *from, size_t len);
char *kn_store_sql_inline(const char *sql, int nparams, const char *const *params, kn_store_escape_fn escape,
                          void *ud);
/* 1 when s is a canonical decimal int64 ("0", "-12": no sign +, no leading zeros or spaces). */
int kn_store_param_int(const char *s, int64_t *out);
/* Column alias type suffix: "name__j" json, "__b" bool, "__i" int, "__f" float, "__t" text.
 * Strips it from name (in place) and returns the type, or -1 when there is none. */
int kn_store_alias_type(char *name);
/* "t"/"true"/"1"/"y"... -> 1, "f"/"false"/"0"... -> 0, else def. */
int kn_store_parse_bool(const char *s, int def);

/* ---- kn_proc helpers (synchronous, inside run()) */
/* First cell as int64 / text (def when no row / NULL / error); frees res. */
int64_t kn_dbconn_scalar_int(kn_dbconn *c, const kn_sql *sql, int nparams, const char *const *params, int64_t def);
/* Same as kn_dbconn_exec_sql but returns NULL (and frees) when it failed, storing the
 * failure in *err (unless err already holds one). */
kn_db_result *kn_dbconn_try(kn_dbconn *c, const kn_sql *sql, int nparams, const char *const *params,
                            kn_db_result **err);

#endif
