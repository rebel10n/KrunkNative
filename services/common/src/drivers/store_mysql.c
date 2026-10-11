/*
 * MySQL / MariaDB driver of the store layer: database.driver = "mysql"
 * (alias "mariadb"; MariaDB Connector/C or libmysqlclient). Tested against
 * MariaDB 11.8; needs MariaDB >= 10.6 or MySQL >= 8.0.
 *
 * Blocking driver: the core runs it on worker threads, one connection each.
 * Sessions run with sql_mode ANSI_QUOTES, PIPES_AS_CONCAT (|| concatenates, as
 * in PostgreSQL / SQLite), NO_BACKSLASH_ESCAPES and STRICT_TRANS_TABLES, UTC
 * time_zone and utf8mb4, READ COMMITTED isolation (PostgreSQL's default: no
 * InnoDB gap locks between the coordinators' concurrent upserts and reapers);
 * CLIENT_FOUND_ROWS makes affected-row counts count matched rows like PostgreSQL. Statements use the text protocol: $n
 * placeholders are replaced by escaped literals ('...' strings; bare integers
 * after LIMIT/OFFSET), so a parameter may be used several times.
 *
 * DDL commits implicitly in MySQL: migrations are not atomic per file here.
 * Native codes map to SQLSTATEs (1062 -> 23505, 2006/2013 -> 08006, 1213 ->
 * 40P01, 1205 -> 55P03, SIGNAL SQLSTATE '45000' -> P0001 user refusal).
 */
#include "../store_internal.h"

#include <mysql.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct my {
    MYSQL *m;
    int broken;
} my;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void lib_init(void) { mysql_library_init(0, NULL, NULL); }

/* ------------------------------------------------------------------ errors */

static int conn_errno(unsigned e) {
    return e == 2006 || e == 2013 || e == 2055 || e == 2002 || e == 2003 || e == 2026 || e == 1927 || e == 4031;
}

static const char *sqlstate_of(unsigned e, const char *native) {
    switch (e) {
    case 1062: case 1586: case 1022: return "23505";
    case 1451: case 1452: case 1216: case 1217: return "23503";
    case 3819: case 4025: return "23514";
    case 1048: case 1364: return "23502";
    case 1213: return "40P01";
    case 1205: case 3572: return "55P03";
    case 1064: case 1149: return "42601";
    case 1146: return "42P01";
    case 1054: return "42703";
    case 1305: case 1630: return "42883";
    case 1050: return "42P07";
    case 1644: return "P0001";
    case 3024: case 1969: case 1317: return "57014";
    default: break;
    }
    if (conn_errno(e)) return "08006";
    return native && *native && strcmp(native, "HY000") ? native : "XX000";
}

static kn_db_result *my_error(my *c) {
    unsigned e = mysql_errno(c->m);
    if (conn_errno(e)) c->broken = 1;
    const char *msg = mysql_error(c->m);
    const char *state = sqlstate_of(e, mysql_sqlstate(c->m));
    kn_db_result *r = kn_db_result_error(state, msg && *msg ? msg : "query failed");
    if (r && !strcmp(state, "P0001")) kn_db_result_set_user_message(r, msg);
    return r;
}

/* ------------------------------------------------------------------ connection */

static int run_simple(MYSQL *m, const char *sql) {
    if (mysql_query(m, sql) != 0) return -1;
    do {
        MYSQL_RES *r = mysql_store_result(m);
        mysql_free_result(r);
    } while (mysql_next_result(m) == 0);
    return mysql_errno(m) ? -1 : 0;
}

static void *c_connect(const kn_db_config *cfg, char *err, size_t errlen) {
    pthread_once(&g_once, lib_init);
    mysql_thread_init();
    MYSQL *m = mysql_init(NULL);
    if (!m) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return NULL;
    }
    unsigned timeout = (unsigned)(cfg->connect_timeout_s > 0 ? cfg->connect_timeout_s : 5);
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    const char *host = cfg->host && *cfg->host ? cfg->host : "127.0.0.1";
    const char *sock = host[0] == '/' ? host : NULL;
    if (!mysql_real_connect(m, sock ? "localhost" : host, cfg->user, cfg->password, cfg->name,
                            (unsigned)(cfg->port > 0 ? cfg->port : 3306), sock,
                            CLIENT_MULTI_STATEMENTS | CLIENT_FOUND_ROWS)) {
        if (err && errlen) snprintf(err, errlen, "%s", mysql_error(m));
        mysql_close(m);
        return NULL;
    }
    if (run_simple(m, "SET SESSION sql_mode = 'ANSI_QUOTES,PIPES_AS_CONCAT,NO_BACKSLASH_ESCAPES,STRICT_TRANS_TABLES,"
                      "NO_ENGINE_SUBSTITUTION,ERROR_FOR_DIVISION_BY_ZERO', time_zone = '+00:00';"
                      " SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED") != 0) {
        if (err && errlen) snprintf(err, errlen, "session setup: %s", mysql_error(m));
        mysql_close(m);
        return NULL;
    }
    if (cfg->statement_timeout_ms > 0) {
        char sql[96];
        /* MariaDB (seconds); MySQL has max_execution_time for SELECTs only: ignore failures */
        snprintf(sql, sizeof sql, "SET SESSION max_statement_time = %.3f", cfg->statement_timeout_ms / 1000.0);
        if (run_simple(m, sql) != 0) {
            snprintf(sql, sizeof sql, "SET SESSION max_execution_time = %d", cfg->statement_timeout_ms);
            run_simple(m, sql);
        }
    }
    my *c = calloc(1, sizeof *c);
    if (!c) {
        mysql_close(m);
        return NULL;
    }
    c->m = m;
    return c;
}

static void c_disconnect(void *conn) {
    my *c = conn;
    mysql_close(c->m);
    free(c);
}

/* ------------------------------------------------------------------ statements */

static size_t my_escape(void *ud, char *to, const char *from, size_t len) {
    return mysql_real_escape_string(((my *)ud)->m, to, from, (unsigned long)len);
}

static char *bind_params(my *c, const char *sql, int n, const char *const *params) {
    return kn_store_sql_inline(sql, n, params, my_escape, c);
}

static kn_db_type field_type(const MYSQL_FIELD *f) {
#ifdef MARIADB_FIELD_ATTR_LAST /* MariaDB Connector/C: extended metadata */
    MARIADB_CONST_STRING attr = {0};
    if (mariadb_field_attr(&attr, f, MARIADB_FIELD_ATTR_FORMAT_NAME) == 0 && attr.length == 4 &&
        !strncmp(attr.str, "json", 4))
        return KN_DB_T_JSON;
#endif
    switch (f->type) {
    case MYSQL_TYPE_TINY: return f->length == 1 ? KN_DB_T_BOOL : KN_DB_T_INT;
    case MYSQL_TYPE_SHORT: case MYSQL_TYPE_LONG: case MYSQL_TYPE_INT24: case MYSQL_TYPE_LONGLONG: case MYSQL_TYPE_YEAR:
        return KN_DB_T_INT;
    case MYSQL_TYPE_FLOAT: case MYSQL_TYPE_DOUBLE: case MYSQL_TYPE_DECIMAL: case MYSQL_TYPE_NEWDECIMAL:
        return KN_DB_T_FLOAT;
    case MYSQL_TYPE_JSON: return KN_DB_T_JSON;
    default: return KN_DB_T_TEXT;
    }
}

static kn_db_result *materialize(MYSQL_RES *rs) {
    unsigned nf = mysql_num_fields(rs);
    MYSQL_FIELD *fs = mysql_fetch_fields(rs);
    const char **names = calloc(nf ? nf : 1, sizeof *names);
    char **nb = calloc(nf ? nf : 1, sizeof *nb);
    kn_db_type *types = calloc(nf ? nf : 1, sizeof *types);
    kn_db_result *res = NULL;
    if (names && nb && types) {
        for (unsigned i = 0; i < nf; i++) {
            nb[i] = strdup(fs[i].name ? fs[i].name : "?column?");
            int t = nb[i] ? kn_store_alias_type(nb[i]) : -1;
            types[i] = t >= 0 ? (kn_db_type)t : field_type(&fs[i]);
            names[i] = nb[i];
        }
        res = kn_db_result_new((int)nf, names, types);
    }
    for (unsigned i = 0; nb && i < nf; i++) free(nb[i]);
    free(nb);
    free(names);
    free(types);
    if (!res) return kn_db_result_error("53200", "out of memory");
    MYSQL_ROW row;
    size_t *lens = calloc(nf ? nf : 1, sizeof *lens);
    while (lens && (row = mysql_fetch_row(rs)) != NULL) {
        unsigned long *l = mysql_fetch_lengths(rs);
        for (unsigned i = 0; i < nf; i++) lens[i] = l ? l[i] : (row[i] ? strlen(row[i]) : 0);
        if (kn_db_result_add_row_n(res, (const char *const *)row, lens) != 0) break;
    }
    free(lens);
    return res;
}

static kn_db_result *c_exec(void *conn, const char *sql_in, int n, const char *const *params) {
    my *c = conn;
    char *sql = bind_params(c, sql_in, n, params);
    if (!sql) return kn_db_result_error("53200", "out of memory");
    int rc = mysql_real_query(c->m, sql, (unsigned long)strlen(sql));
    free(sql);
    if (rc != 0) return my_error(c);
    kn_db_result *res = NULL;
    MYSQL_RES *rs = mysql_store_result(c->m);
    if (rs) {
        res = materialize(rs);
        mysql_free_result(rs);
    } else if (mysql_field_count(c->m) != 0) {
        return my_error(c);
    } else {
        res = kn_db_result_command((int64_t)mysql_affected_rows(c->m));
        if (res) kn_db_result_set_last_id(res, (int64_t)mysql_insert_id(c->m));
    }
    /* drain further result sets (CALL procedures) */
    while (mysql_more_results(c->m) && mysql_next_result(c->m) == 0) mysql_free_result(mysql_store_result(c->m));
    if (mysql_errno(c->m)) {
        kn_db_result_free(res);
        return my_error(c);
    }
    return res ? res : kn_db_result_error("53200", "out of memory");
}

static int c_exec_script(void *conn, const char *sql, char *err, size_t errlen) {
    my *c = conn;
    if (mysql_real_query(c->m, sql, (unsigned long)strlen(sql)) != 0) goto fail;
    for (;;) {
        MYSQL_RES *r = mysql_store_result(c->m);
        mysql_free_result(r);
        int nx = mysql_next_result(c->m);
        if (nx > 0) goto fail;
        if (nx < 0) break;
    }
    return 0;
fail:
    if (conn_errno(mysql_errno(c->m))) c->broken = 1;
    if (err && errlen) snprintf(err, errlen, "%s", mysql_error(c->m));
    return -1;
}

static int c_broken(void *conn) { return ((my *)conn)->broken; }

static int c_lock(void *conn, const char *name, int on) {
    kn_db_result *r = c_exec(conn, on ? "SELECT GET_LOCK($1, 120)" : "SELECT RELEASE_LOCK($1)", KN_DB_ARGS(name));
    int ok = kn_db_ok(r) && (!on || kn_db_get_int64(r, 0, 0, 0) == 1);
    kn_db_result_free(r);
    return ok ? 0 : -1;
}

static void *c_native(void *conn) { return ((my *)conn)->m; }

static const kn_store_blocking_ops MY_BLOCKING = {
    c_connect, c_disconnect, c_exec, c_exec_script, c_broken, c_lock, NULL, c_native, "START TRANSACTION"};

static const kn_sql_macro MY_MACROS[] = {
    {"now", "NOW(3)"},
    {"ago", "(NOW(3) - INTERVAL (%1) SECOND)"},
    {"later", "(NOW(3) + INTERVAL (%1) SECOND)"},
    {"epoch_ms", "CAST(ROUND(UNIX_TIMESTAMP(%1) * 1000) AS SIGNED)"},
    {"from_ms", "FROM_UNIXTIME((%1) / 1000)"},
    {"int", "CAST(%1 AS SIGNED)"},
    {"float", "CAST(%1 AS DOUBLE)"},
    {"text", "CAST(%1 AS CHAR)"},
    {"bool", "(LOWER(CAST(%1 AS CHAR)) IN ('1', 't', 'true', 'y', 'yes', 'on'))"},
    {"json", "JSON_EXTRACT(%1, '$')"},
    {"jagg", "JSON_ARRAYAGG(%*)"},
    {"html", "REPLACE(REPLACE(REPLACE(REPLACE(REPLACE(COALESCE(%1, ''), '&', '&amp;'), '<', '&lt;'), '>', '&gt;'),"
             " '\"', '&quot;'), '''', '&#39;')"},
    {"hash", "SHA2(%1, 256)"},
    {"greatest", "GREATEST(%*)"},
    {"least", "LEAST(%*)"},
    {"insert_ignore", "INSERT IGNORE"},
    {"ignore_conflicts", ""},
    {"for_update", "FOR UPDATE"},
    {"serial_pk", "INT AUTO_INCREMENT PRIMARY KEY"},
    {"bigserial_pk", "BIGINT AUTO_INCREMENT PRIMARY KEY"},
    {"ilike", "LIKE"},
    {"true", "1"},
    {"false", "0"},
    {NULL, NULL}};

const kn_store_driver kn_store_driver_mysql = {
    .name = "mysql",
    .dialect = "mysql",
    .caps = 0,
    .default_port = 3306,
    .default_pool = 4,
    .macros = MY_MACROS,
    .blocking = &MY_BLOCKING,
    .async = NULL,
};
