/*
 * SQLite driver of the store layer: database.driver = "sqlite" (system or
 * bundled libsqlite3, >= 3.35 for RETURNING / math functions).
 *
 * Zero-config default for small hosts: one file (database.path, default
 * data/coordinator.db, created with its directory). Blocking driver: the core
 * runs it on worker threads (one connection each), so the loop never blocks.
 * Connections use WAL (readers do not block the writer), foreign keys on,
 * busy_timeout = database.busy_timeout_ms; transactions are BEGIN IMMEDIATE so
 * several coordinator processes can share one file safely (writers serialise).
 *
 * $n placeholders become ?n; canonical integers bind as INTEGER, everything
 * else as TEXT. Results are materialized; column types come from the declared
 * column type (BOOLEAN, JSON, INTEGER, REAL...), the alias suffix (__j __b __i
 * __f __t) or the value's storage class. RAISE(ABORT, msg) from triggers maps
 * to SQLSTATE P0001 (a user-facing refusal), constraint codes to 23xxx.
 * Registered functions: kn_sha256(text) -> hex.
 */
#include "../store_internal.h"
#include "knsvc/crypto.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define CACHE_SIZE 48

typedef struct lite_stmt {
    char *sql;
    sqlite3_stmt *st;
    unsigned long long used;
} lite_stmt;

typedef struct lite {
    sqlite3 *db;
    lite_stmt cache[CACHE_SIZE];
    unsigned long long tick;
} lite;

/* ------------------------------------------------------------------ errors */

static const char *sqlstate_of(sqlite3 *db, int rc, const char *msg) {
    int ext = sqlite3_extended_errcode(db);
    switch (rc & 0xff) {
    case SQLITE_CONSTRAINT:
        switch (ext) {
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_PRIMARYKEY: return "23505";
        case SQLITE_CONSTRAINT_FOREIGNKEY: return "23503";
        case SQLITE_CONSTRAINT_CHECK: return "23514";
        case SQLITE_CONSTRAINT_NOTNULL: return "23502";
        case SQLITE_CONSTRAINT_TRIGGER: return "P0001";
        default: return "23000";
        }
    case SQLITE_BUSY:
    case SQLITE_LOCKED: return "40001";
    case SQLITE_INTERRUPT: return "57014";
    case SQLITE_FULL: return "53100";
    case SQLITE_NOMEM: return "53200";
    case SQLITE_READONLY:
    case SQLITE_PERM:
    case SQLITE_AUTH: return "42501";
    case SQLITE_CANTOPEN:
    case SQLITE_IOERR:
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB: return "58030";
    case SQLITE_MISMATCH:
    case SQLITE_TOOBIG:
    case SQLITE_RANGE: return "22000";
    default: break;
    }
    if (msg) {
        if (strstr(msg, "no such table")) return "42P01";
        if (strstr(msg, "no such column")) return "42703";
        if (strstr(msg, "no such function")) return "42883";
        if (strstr(msg, "syntax error") || strstr(msg, "incomplete input")) return "42601";
        if (strstr(msg, "already exists")) return "42P07";
    }
    return "XX000";
}

static kn_db_result *lite_error(sqlite3 *db, int rc) {
    const char *msg = sqlite3_errmsg(db);
    const char *state = sqlstate_of(db, rc, msg);
    kn_db_result *r = kn_db_result_error(state, msg);
    if (r && !strcmp(state, "P0001")) kn_db_result_set_user_message(r, msg);
    return r;
}

/* ------------------------------------------------------------------ functions */

static void fn_sha256(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    const unsigned char *t = sqlite3_value_text(argv[0]);
    int n = sqlite3_value_bytes(argv[0]);
    uint8_t d[KN_SHA256_LEN];
    char hex[2 * KN_SHA256_LEN + 1];
    kn_sha256(t, (size_t)n, d);
    kn_hex_encode(d, sizeof d, hex, sizeof hex);
    sqlite3_result_text(ctx, hex, -1, SQLITE_TRANSIENT);
}

/* ------------------------------------------------------------------ connection */

static int mkdirs_for(const char *path) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return 0;
    *slash = 0;
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(dir, 0750) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(dir, 0750) != 0 && errno != EEXIST ? -1 : 0;
}

static void *c_connect(const kn_db_config *cfg, char *err, size_t errlen) {
    const char *path = cfg->path && *cfg->path ? cfg->path : "data/coordinator.db";
    int memory = !strcmp(path, ":memory:");
    if (!memory && strncmp(path, "file:", 5) != 0 && mkdirs_for(path) != 0) {
        if (err && errlen) snprintf(err, errlen, "cannot create the directory of %s: %s", path, strerror(errno));
        return NULL;
    }
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI,
                             NULL);
    if (rc != SQLITE_OK) {
        if (err && errlen) snprintf(err, errlen, "%s: %s", path, db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, cfg->busy_timeout_ms > 0 ? cfg->busy_timeout_ms : 5000);
    char *e = NULL;
    rc = sqlite3_exec(db, memory ? "PRAGMA foreign_keys = ON"
                                 : "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON",
                      NULL, NULL, &e);
    if (rc == SQLITE_OK)
        rc = sqlite3_create_function_v2(db, "kn_sha256", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fn_sha256, NULL,
                                        NULL, NULL);
    if (rc != SQLITE_OK) {
        if (err && errlen) snprintf(err, errlen, "%s: %s", path, e ? e : sqlite3_errmsg(db));
        sqlite3_free(e);
        sqlite3_close(db);
        return NULL;
    }
    lite *l = calloc(1, sizeof *l);
    if (!l) {
        sqlite3_close(db);
        return NULL;
    }
    l->db = db;
    return l;
}

static void c_disconnect(void *conn) {
    lite *l = conn;
    for (int i = 0; i < CACHE_SIZE; i++) {
        sqlite3_finalize(l->cache[i].st);
        free(l->cache[i].sql);
    }
    sqlite3_close(l->db);
    free(l);
}

/* Prepared statement for sql from the cache (prepared on a miss). */
static sqlite3_stmt *stmt_for(lite *l, const char *sql, int *rc) {
    int victim = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (l->cache[i].sql && !strcmp(l->cache[i].sql, sql)) {
            l->cache[i].used = ++l->tick;
            *rc = SQLITE_OK;
            return l->cache[i].st;
        }
        if (l->cache[i].used < l->cache[victim].used) victim = i;
    }
    sqlite3_stmt *st = NULL;
    const char *tail = NULL;
    *rc = sqlite3_prepare_v3(l->db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, &tail);
    if (*rc != SQLITE_OK) return NULL;
    if (!st) { /* empty statement */
        *rc = SQLITE_OK;
        return NULL;
    }
    char *copy = strdup(sql);
    if (!copy) return st; /* uncached: caller finalizes when it is not in the cache */
    sqlite3_finalize(l->cache[victim].st);
    free(l->cache[victim].sql);
    l->cache[victim].sql = copy;
    l->cache[victim].st = st;
    l->cache[victim].used = ++l->tick;
    return st;
}

static int cached(const lite *l, const sqlite3_stmt *st) {
    for (int i = 0; i < CACHE_SIZE; i++)
        if (l->cache[i].st == st) return 1;
    return 0;
}

static kn_db_type decl_type(const char *decl) {
    if (!decl) return (kn_db_type)-1;
    if (!strncasecmp(decl, "BOOL", 4)) return KN_DB_T_BOOL;
    if (!strncasecmp(decl, "JSON", 4)) return KN_DB_T_JSON;
    if (strcasestr(decl, "INT")) return KN_DB_T_INT;
    if (strcasestr(decl, "REAL") || strcasestr(decl, "FLOA") || strcasestr(decl, "DOUB") || strcasestr(decl, "NUMERIC"))
        return KN_DB_T_FLOAT;
    if (strcasestr(decl, "BLOB")) return KN_DB_T_BYTES;
    return KN_DB_T_TEXT;
}

static kn_db_result *c_exec(void *conn, const char *sql_in, int n, const char *const *params) {
    lite *l = conn;
    char *sql = kn_store_sql_numbered(sql_in);
    if (!sql) return kn_db_result_error("53200", "out of memory");
    int rc;
    sqlite3_stmt *st = stmt_for(l, sql, &rc);
    free(sql);
    if (rc != SQLITE_OK) return lite_error(l->db, rc);
    if (!st) return kn_db_result_command(0);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    int nb = sqlite3_bind_parameter_count(st);
    for (int i = 1; i <= nb; i++) {
        const char *name = sqlite3_bind_parameter_name(st, i);
        int idx = name && name[0] == '?' ? atoi(name + 1) : i;
        const char *v = idx >= 1 && idx <= n ? params[idx - 1] : NULL;
        int64_t iv;
        if (!v) rc = sqlite3_bind_null(st, i);
        else if (kn_store_param_int(v, &iv)) rc = sqlite3_bind_int64(st, i, iv);
        else rc = sqlite3_bind_text(st, i, v, -1, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) break;
    }
    int ncols = sqlite3_column_count(st);
    kn_db_result *res = NULL;
    kn_db_type *forced = NULL;
    int *seen = NULL;
    if (rc == SQLITE_OK && ncols > 0) {
        const char **names = calloc((size_t)ncols, sizeof *names);
        char **namebuf = calloc((size_t)ncols, sizeof *namebuf);
        forced = calloc((size_t)ncols, sizeof *forced);
        seen = calloc((size_t)ncols, sizeof *seen);
        kn_db_type *types = calloc((size_t)ncols, sizeof *types);
        if (names && namebuf && forced && seen && types) {
            for (int c = 0; c < ncols; c++) {
                namebuf[c] = strdup(sqlite3_column_name(st, c) ? sqlite3_column_name(st, c) : "?column?");
                int t = namebuf[c] ? kn_store_alias_type(namebuf[c]) : -1;
                if (t < 0) t = (int)decl_type(sqlite3_column_decltype(st, c));
                forced[c] = (kn_db_type)t;
                seen[c] = t >= 0;
                types[c] = t >= 0 ? (kn_db_type)t : KN_DB_T_TEXT;
                names[c] = namebuf[c];
            }
            res = kn_db_result_new(ncols, names, types);
        }
        for (int c = 0; namebuf && c < ncols; c++) free(namebuf[c]);
        free(namebuf);
        free(names);
        free(types);
        if (!res) {
            free(forced);
            free(seen);
            if (!cached(l, st)) sqlite3_finalize(st);
            return kn_db_result_error("53200", "out of memory");
        }
    }
    const char **cells = ncols > 0 ? calloc((size_t)ncols, sizeof *cells) : NULL;
    size_t *lens = ncols > 0 ? calloc((size_t)ncols, sizeof *lens) : NULL;
    while (rc == SQLITE_OK || rc == SQLITE_ROW) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) break;
        if (!res || !cells || !lens) continue;
        for (int c = 0; c < ncols; c++) {
            int t = sqlite3_column_type(st, c);
            if (t == SQLITE_NULL) {
                cells[c] = NULL;
                continue;
            }
            if (!seen[c]) {
                seen[c] = 1;
                kn_db_result_set_type(res, c, t == SQLITE_INTEGER ? KN_DB_T_INT : t == SQLITE_FLOAT ? KN_DB_T_FLOAT
                                                                                                 : KN_DB_T_TEXT);
            }
            cells[c] = (const char *)sqlite3_column_text(st, c);
            lens[c] = (size_t)sqlite3_column_bytes(st, c);
            if (!cells[c]) cells[c] = "";
        }
        if (kn_db_result_add_row_n(res, cells, lens) != 0) {
            rc = SQLITE_NOMEM;
            break;
        }
    }
    free(cells);
    free(lens);
    free(forced);
    free(seen);
    int ro = sqlite3_stmt_readonly(st);
    if (rc != SQLITE_DONE) {
        kn_db_result *e = lite_error(l->db, rc);
        kn_db_result_free(res);
        sqlite3_reset(st);
        if (!cached(l, st)) sqlite3_finalize(st);
        return e;
    }
    sqlite3_reset(st);
    if (!cached(l, st)) sqlite3_finalize(st);
    if (!res) res = kn_db_result_command(0);
    if (!res) return kn_db_result_error("53200", "out of memory");
    if (!ro) {
        kn_db_result_set_affected(res, sqlite3_changes64(l->db));
        kn_db_result_set_last_id(res, sqlite3_last_insert_rowid(l->db));
    }
    return res;
}

static int c_exec_script(void *conn, const char *sql, char *err, size_t errlen) {
    lite *l = conn;
    char *e = NULL;
    int rc = sqlite3_exec(l->db, sql, NULL, NULL, &e);
    if (rc != SQLITE_OK && err && errlen) snprintf(err, errlen, "%s", e ? e : sqlite3_errmsg(l->db));
    sqlite3_free(e);
    return rc == SQLITE_OK ? 0 : -1;
}

static int c_broken(void *conn) { return 0; }
static void c_interrupt(void *conn) { sqlite3_interrupt(((lite *)conn)->db); }
static void *c_native(void *conn) { return ((lite *)conn)->db; }

static const kn_store_blocking_ops LITE_BLOCKING = {
    c_connect, c_disconnect, c_exec, c_exec_script, c_broken, NULL, c_interrupt, c_native, "BEGIN IMMEDIATE"};

#define TS_FMT "'%Y-%m-%d %H:%M:%f'"

static const kn_sql_macro LITE_MACROS[] = {
    {"now", "strftime(" TS_FMT ", 'now')"},
    {"ago", "strftime(" TS_FMT ", 'now', '-' || (%1) || ' seconds')"},
    {"later", "strftime(" TS_FMT ", 'now', '+' || (%1) || ' seconds')"},
    {"epoch_ms", "CAST(ROUND((julianday(%1) - 2440587.5) * 86400000) AS INTEGER)"},
    {"from_ms", "strftime(" TS_FMT ", (%1) / 1000.0, 'unixepoch')"},
    {"int", "CAST(%1 AS INTEGER)"},
    {"float", "CAST(%1 AS REAL)"},
    {"text", "CAST(%1 AS TEXT)"},
    {"bool", "(lower(CAST(%1 AS TEXT)) IN ('1', 't', 'true', 'y', 'yes', 'on'))"},
    {"json", "json(%1)"},
    {"jagg", "json_group_array(%*)"},
    {"html", "replace(replace(replace(replace(replace(coalesce(%1, ''), '&', '&amp;'), '<', '&lt;'), '>', '&gt;'),"
             " '\"', '&quot;'), '''', '&#39;')"},
    {"hash", "kn_sha256(%1)"},
    {"greatest", "max(%*)"},
    {"least", "min(%*)"},
    {"insert_ignore", "INSERT OR IGNORE"},
    {"ignore_conflicts", ""},
    {"for_update", ""},
    {"serial_pk", "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"bigserial_pk", "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"ilike", "LIKE"},
    {"true", "1"},
    {"false", "0"},
    {NULL, NULL}};

const kn_store_driver kn_store_driver_sqlite = {
    .name = "sqlite",
    .dialect = "sqlite",
    .caps = KN_DB_CAP_RETURNING | KN_DB_CAP_TX_DDL,
    .default_port = 0,
    .default_pool = 2,
    .macros = LITE_MACROS,
    .blocking = &LITE_BLOCKING,
    .async = NULL,
};
