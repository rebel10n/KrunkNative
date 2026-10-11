/*
 * Store layer on one driver (knsvc/db.h + knsvc/store.h): typed results, error
 * classes, kn_sql macros, pool concurrency, transactions (incl. the aborted
 * transaction rule), kn_proc, prepared statements, pub/sub, kn_dbconn,
 * migrations with a dialect subdirectory, and kn_db_free with queued work.
 *
 *   knsvc_test_store sqlite              temp file, always available
 *   knsvc_test_store mysql               KN_TEST_MYSQL_HOST/PORT/USER/PASSWORD (default
 *                                        127.0.0.1:3306 root); own database, dropped after
 *   knsvc_test_store postgres            config/config.json credentials, database
 *                                        KN_TEST_DATABASE (krunknative_svccore), own schema
 * Exit 77 (CTest skip) when the driver is not built or its server is unreachable.
 */
#include "kn_test.h"
#include "knsvc/config.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knsvc/migrate.h"
#include "knsvc/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static kn_loop *loop;
static int pending;
static const char *driver;
static char scratch[512];      /* sqlite temp dir / mysql database / pg schema */

static void run_until_done(int timeout_ms) {
    uint64_t end = kn_now_ms() + (uint64_t)timeout_ms;
    while (pending > 0 && kn_now_ms() < end) kn_loop_run_once(loop, 20);
    CHECK_EQ_INT(pending, 0);
}

static void exec_or_die(kn_dbconn *c, const char *sql) {
    char err[512];
    if (kn_dbconn_exec_script(c, sql, err, sizeof err) != 0) {
        fprintf(stderr, "setup failed: %s: %s\n", sql, err);
        exit(1);
    }
}

/* ------------------------------------------------------------------ setup per driver */

static kn_config *g_cfg;

static int setup(kn_db_config *dbc) {
    memset(dbc, 0, sizeof *dbc);
    if (!kn_store_driver_find(driver)) {
        printf("SKIP: driver %s not built in\n", driver);
        return 77;
    }
    dbc->driver = driver;
    dbc->connect_timeout_s = 3;
    dbc->notify_poll_ms = 50;
    char err[512];
    if (!strcmp(driver, "sqlite")) {
        snprintf(scratch, sizeof scratch, "/tmp/kn-store-test-XXXXXX");
        if (!mkdtemp(scratch)) return 1;
        static char path[600];
        snprintf(path, sizeof path, "%s/sub/dir/test.db", scratch); /* directories are created */
        dbc->path = path;
        dbc->pool_size = 3;
        return 0;
    }
    if (!strcmp(driver, "mysql")) {
        const char *h = getenv("KN_TEST_MYSQL_HOST"), *p = getenv("KN_TEST_MYSQL_PORT");
        const char *u = getenv("KN_TEST_MYSQL_USER"), *pw = getenv("KN_TEST_MYSQL_PASSWORD");
        dbc->host = h && *h ? h : "127.0.0.1";
        dbc->port = p && *p ? atoi(p) : 3306;
        dbc->user = u && *u ? u : "root";
        dbc->password = pw ? pw : "";
        dbc->pool_size = 4;
        kn_dbconn *admin = kn_dbconn_open(dbc, err, sizeof err);
        if (!admin) {
            printf("SKIP: cannot connect to MySQL/MariaDB at %s:%d: %s\n", dbc->host, dbc->port, err);
            return 77;
        }
        snprintf(scratch, sizeof scratch, "kn_test_store_%d", (int)getpid());
        char sql[700];
        snprintf(sql, sizeof sql, "DROP DATABASE IF EXISTS %s; CREATE DATABASE %s CHARACTER SET utf8mb4", scratch, scratch);
        exec_or_die(admin, sql);
        kn_dbconn_close(admin);
        dbc->name = scratch;
        return 0;
    }
    /* postgres */
    char path[4096];
    snprintf(path, sizeof path, "%s/config/config.json", KN_REPO_ROOT);
    g_cfg = kn_config_load(path, 0, NULL, err, sizeof err);
    if (!g_cfg) g_cfg = kn_config_from_string("{}", err, sizeof err);
    const char *name = getenv("KN_TEST_DATABASE");
    kn_config_set(g_cfg, "database.name", name && *name ? name : "krunknative_svccore");
    kn_config_set(g_cfg, "database.driver", "postgres");
    kn_db_config_from(g_cfg, dbc);
    dbc->notify_poll_ms = 50;
    kn_dbconn *admin = kn_dbconn_open(dbc, err, sizeof err);
    if (!admin) {
        printf("SKIP: cannot connect to PostgreSQL: %s\n", err);
        return 77;
    }
    snprintf(scratch, sizeof scratch, "kn_test_store_%d", (int)getpid());
    char sql[700];
    snprintf(sql, sizeof sql, "DROP SCHEMA IF EXISTS %s CASCADE; CREATE SCHEMA %s", scratch, scratch);
    exec_or_die(admin, sql);
    kn_dbconn_close(admin);
    dbc->search_path = scratch;
    return 0;
}

static void teardown(const kn_db_config *dbc) {
    char err[512], sql[700];
    if (!strcmp(driver, "sqlite")) {
        snprintf(sql, sizeof sql, "rm -rf %s", scratch);
        CHECK(system(sql) == 0);
        return;
    }
    kn_db_config a = *dbc;
    if (!strcmp(driver, "mysql")) a.name = NULL;
    else a.search_path = NULL;
    kn_dbconn *admin = kn_dbconn_open(&a, err, sizeof err);
    if (!admin) return;
    if (!strcmp(driver, "mysql")) snprintf(sql, sizeof sql, "DROP DATABASE IF EXISTS %s", scratch);
    else snprintf(sql, sizeof sql, "DROP SCHEMA IF EXISTS %s CASCADE", scratch);
    exec_or_die(admin, sql);
    kn_dbconn_close(admin);
}

static const kn_sql DDL = {
    .pg = "CREATE TABLE accounts (id serial PRIMARY KEY, name text UNIQUE NOT NULL, kr bigint NOT NULL DEFAULT 0,"
          " verified boolean NOT NULL DEFAULT false, bio text, stats jsonb, ratio double precision,"
          " created timestamptz NOT NULL DEFAULT now())",
    .sqlite = "CREATE TABLE accounts (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE NOT NULL,"
              " kr INTEGER NOT NULL DEFAULT 0, verified BOOLEAN NOT NULL DEFAULT 0, bio TEXT, stats JSON, ratio REAL,"
              " created TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')))",
    .mysql = "CREATE TABLE accounts (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100) NOT NULL UNIQUE,"
             " kr BIGINT NOT NULL DEFAULT 0, verified BOOLEAN NOT NULL DEFAULT 0, bio TEXT, stats JSON, ratio DOUBLE,"
             " created DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3))"};

/* ------------------------------------------------------------------ basic */

static const kn_sql INS = {.any = "INSERT INTO accounts (name, kr, verified, bio, stats, ratio) VALUES ($1, $2, $3, $4, $5, $6)"};
static const kn_sql INS_NAME = {.any = "INSERT INTO accounts (name) VALUES ($1)"};

static void on_insert(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_INT(kn_db_affected(r), 1);
    if (strcmp(driver, "postgres")) CHECK(kn_db_last_id(r) > 0);
    pending--;
}

static void on_select(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_INT(kn_db_rows(r), 2);
    CHECK_EQ_STR(kn_db_get_by(r, 0, "name"), "alice");
    CHECK_EQ_STR(kn_db_get_by(r, 1, "name"), "bob'); DROP TABLE accounts; -- \\' \"q\" $1 ?");
    int kr = kn_db_col(r, "kr");
    CHECK_EQ_INT(kn_db_get_int64(r, 0, kr, 0), 150);
    CHECK(kn_db_isnull(r, 1, kn_db_col(r, "bio")));
    CHECK_EQ_INT(kn_db_get_bool(r, 0, kn_db_col(r, "verified"), -1), 1);
    CHECK_EQ_INT(kn_db_get_bool(r, 1, kn_db_col(r, "verified"), -1), 0);
    CHECK(kn_db_col(r, "nope") == -1 && kn_db_get_by(r, 0, "nope") == NULL);
    CHECK_EQ_INT(kn_db_coltype(r, kr), KN_DB_T_INT);
    cJSON *j = kn_db_result_to_json(r);
    char *s = cJSON_PrintUnformatted(j);
    CHECK_EQ_STR(s, "[{\"id\":1,\"name\":\"alice\",\"kr\":150,\"verified\":true,\"bio\":\"hi\",\"stats\":{\"kills\":3},\"ratio\":0.5},"
                    "{\"id\":2,\"name\":\"bob'); DROP TABLE accounts; -- \\\\' \\\"q\\\" $1 ?\",\"kr\":0,\"verified\":false,"
                    "\"bio\":null,\"stats\":null,\"ratio\":null}]");
    free(s);
    cJSON_Delete(j);
    pending--;
}

static void on_dup(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    CHECK(kn_db_is_unique_violation(r));
    CHECK_EQ_INT(kn_db_errclass_of(r), KN_DB_E_UNIQUE);
    CHECK_EQ_STR(kn_db_sqlstate(r), "23505");
    pending--;
}

static void on_syntax(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    CHECK_EQ_INT(kn_db_errclass_of(r), KN_DB_E_SYNTAX);
    pending--;
}

static void on_typed(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    cJSON *j = kn_db_row_to_json(r, 0);
    cJSON_DeleteItemFromObject(j, "hash");
    cJSON_DeleteItemFromObject(j, "age_ms");
    cJSON_DeleteItemFromObject(j, "now_ms");
    char *s = cJSON_PrintUnformatted(j);
    CHECK_EQ_STR(s, "{\"n\":7,\"b\":true,\"o\":{\"a\":[1,2]},\"t\":\"5\",\"h\":\"&lt;b&gt; &amp; &quot;x&quot; &#39;y&#39;\"}");
    free(s);
    cJSON_Delete(j);
    /* {hash}: hex sha256 of "abc" (postgres keeps bytea, its text form is \x...) */
    const char *h = kn_db_get_by(r, 0, "hash");
    CHECK(h && strstr(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != NULL);
    int64_t age = kn_db_get_int64(r, 0, kn_db_col(r, "age_ms"), -1);
    CHECK(age >= 59000 && age <= 61000);
    int64_t now_ms = kn_db_get_int64(r, 0, kn_db_col(r, "now_ms"), 0);
    int64_t d = now_ms - kn_wall_ms();
    CHECK(d > -5000 && d < 5000);
    pending--;
}

static const kn_sql TYPED = {
    .any = "SELECT {int:$1} + 2 AS n__i, {true} AS b__b, '{\"a\":[1,2]}' AS o__j, {text:$2} AS t__t, {html:$3} AS h,"
           " {hash:'abc'} AS hash, {epoch_ms:{now}} - {epoch_ms:{ago:60}} AS age_ms, {epoch_ms:{now}} AS now_ms"};

static void test_basic(kn_db *db) {
    kn_db_query_sql(db, &INS, KN_DB_ARGS("alice", "150", "1", "hi", "{\"kills\":3}", "0.5"), on_insert, NULL);
    pending = 1;
    run_until_done(5000);
    kn_db_query_sql(db, &INS_NAME, KN_DB_ARGS("bob'); DROP TABLE accounts; -- \\' \"q\" $1 ?"), on_insert, NULL);
    pending = 1;
    run_until_done(5000);
    kn_db_query(db, "SELECT id, name, kr, verified, bio, stats, ratio FROM accounts ORDER BY id", 0, NULL, on_select, NULL);
    kn_db_query_sql(db, &INS_NAME, KN_DB_ARGS("alice"), on_dup, NULL);
    kn_db_query(db, "SELEC 1", 0, NULL, on_syntax, NULL);
    kn_db_query_sql(db, &TYPED, KN_DB_ARGS("5", "5", "<b> & \"x\" 'y'"), on_typed, NULL);
    pending = 4;
    run_until_done(5000);
}

/* ------------------------------------------------------------------ concurrency */

static int conc_ok;
static void on_conc(kn_db_result *r, void *ud) {
    if (kn_db_ok(r)) conc_ok++;
    else fprintf(stderr, "concurrent insert: %s\n", kn_db_error(r));
    pending--;
}

static void on_count(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_STR(kn_db_get(r, 0, 0), (const char *)ud);
    pending--;
}

static void test_concurrency(kn_db *db) {
    char name[32];
    for (int i = 0; i < 60; i++) {
        snprintf(name, sizeof name, "c%02d", i);
        kn_db_query_sql(db, &INS_NAME, KN_DB_ARGS(name), on_conc, NULL);
    }
    pending = 60;
    run_until_done(20000);
    CHECK_EQ_INT(conc_ok, 60);
    kn_db_query(db, "SELECT count(*) FROM accounts WHERE name LIKE 'c%'", 0, NULL, on_count, "60");
    pending = 1;
    run_until_done(5000);
}

/* ------------------------------------------------------------------ prepared */

static void on_prepared(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_STR(kn_db_get(r, 0, 0), (const char *)ud);
    pending--;
}

static void on_bad(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    pending--;
}

static void test_prepared(kn_db *db) {
    CHECK(kn_db_prepare(db, "by_name", "SELECT name FROM accounts WHERE name = $1") == 0);
    CHECK(kn_db_prepare(db, "by_name", "SELECT 1") != 0);
    for (int i = 0; i < 6; i++) kn_db_exec_prepared(db, "by_name", KN_DB_ARGS("alice"), on_prepared, "alice");
    kn_db_exec_prepared(db, "unknown_stmt", 0, NULL, on_bad, NULL);
    pending = 7;
    run_until_done(5000);
}

/* ------------------------------------------------------------------ transactions */

static int tx_step;
static void tx_end_cb(kn_db_result *r, void *ud) {
    CHECK_EQ_INT(kn_db_ok(r), ud != NULL);
    if (!ud) CHECK_EQ_INT(kn_db_errclass_of(r), KN_DB_E_TX_ABORTED);
    pending--;
}
static void tx_ok_cb(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    tx_step++;
}
static void tx_fail_cb(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    tx_step++;
}

static void tx_commit_begun(kn_db_tx *tx, kn_db_result *r, void *ud) {
    CHECK(tx != NULL && kn_db_ok(r));
    kn_db_tx_query_sql(tx, &INS_NAME, KN_DB_ARGS("tx-commit"), tx_ok_cb, NULL);
    kn_db_tx_query(tx, "SELECT count(*) FROM accounts WHERE name = 'tx-commit'", 0, NULL, on_count, "1");
    kn_db_tx_commit(tx, tx_end_cb, (void *)1);
}

static void tx_rollback_begun(kn_db_tx *tx, kn_db_result *r, void *ud) {
    CHECK(tx != NULL);
    kn_db_tx_query_sql(tx, &INS_NAME, KN_DB_ARGS("tx-rollback"), tx_ok_cb, NULL);
    kn_db_tx_rollback(tx, tx_end_cb, (void *)1);
}

static void tx_failing_begun(kn_db_tx *tx, kn_db_result *r, void *ud) {
    CHECK(tx != NULL);
    kn_db_tx_query_sql(tx, &INS_NAME, KN_DB_ARGS("tx-failed"), tx_ok_cb, NULL);
    kn_db_tx_query_sql(tx, &INS_NAME, KN_DB_ARGS("alice"), tx_fail_cb, NULL); /* dup */
    kn_db_tx_query(tx, "SELECT 1", 0, NULL, tx_fail_cb, NULL);              /* aborted transaction */
    kn_db_tx_commit(tx, tx_end_cb, NULL);                                    /* reports the failure */
}

static void test_transactions(kn_db *db) {
    tx_step = 0;
    kn_db_begin(db, tx_commit_begun, NULL);
    pending = 2;
    run_until_done(5000);
    kn_db_begin(db, tx_rollback_begun, NULL);
    pending = 1;
    run_until_done(5000);
    kn_db_begin(db, tx_failing_begun, NULL);
    pending = 1;
    run_until_done(5000);
    kn_db_query(db, "SELECT count(*) FROM accounts WHERE name IN ('tx-rollback', 'tx-failed')", 0, NULL, on_count, "0");
    kn_db_query(db, "SELECT count(*) FROM accounts WHERE name = 'tx-commit'", 0, NULL, on_count, "1");
    pending = 2;
    run_until_done(5000);
    CHECK_EQ_INT(tx_step, 5);
}

/* ------------------------------------------------------------------ kn_proc */

static const kn_sql ADD_KR = {.any = "UPDATE accounts SET kr = kr + {int:$2} WHERE name = $1 AND kr + {int:$2} >= 0"};
static const kn_sql GET_KR = {.any = "SELECT kr FROM accounts WHERE name = $1"};

/* give(from, to, amount): both updates or neither; refusal is a user error */
static kn_db_result *proc_give(kn_dbconn *c, int n, const char *const *p) {
    char neg[32];
    snprintf(neg, sizeof neg, "-%s", p[2]);
    kn_db_result *err = NULL;
    kn_db_result *r = kn_dbconn_try(c, &ADD_KR, KN_DB_ARGS(p[1], p[2]), &err);
    if (!r) return err;
    kn_db_result_free(r);
    r = kn_dbconn_try(c, &ADD_KR, KN_DB_ARGS(p[0], neg), &err);
    if (!r) return err;
    int64_t done = kn_db_affected(r);
    kn_db_result_free(r);
    if (done != 1) return kn_db_result_user_error("Not enough KR");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "funds", (double)kn_dbconn_scalar_int(c, &GET_KR, KN_DB_ARGS(p[0]), -1));
    return kn_db_result_json("give", o);
}

static const kn_proc GIVE = {"give", NULL, proc_give};

static void on_give_ok(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_STR(kn_db_get(r, 0, 0), "{\"funds\":100}");
    CHECK_EQ_INT(kn_db_coltype(r, 0), KN_DB_T_JSON);
    pending--;
}

static void on_give_refused(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    CHECK_EQ_INT(kn_db_errclass_of(r), KN_DB_E_USER);
    CHECK_EQ_STR(kn_db_user_message(r), "Not enough KR");
    pending--;
}

static void tx_proc_begun(kn_db_tx *tx, kn_db_result *r, void *ud) {
    CHECK(tx != NULL);
    kn_db_tx_call(tx, &GIVE, KN_DB_ARGS("alice", "c01", "10"), tx_ok_cb, NULL);
    kn_db_tx_rollback(tx, tx_end_cb, (void *)1);
}

static void test_procs(kn_db *db) {
    kn_db_call(db, &GIVE, KN_DB_ARGS("alice", "c00", "50"), on_give_ok, NULL);
    pending = 1;
    run_until_done(5000);
    kn_db_call(db, &GIVE, KN_DB_ARGS("alice", "c00", "500"), on_give_refused, NULL);
    pending = 1;
    run_until_done(5000);
    /* the refused call rolled back its first update */
    kn_db_query_sql(db, &GET_KR, KN_DB_ARGS("c00"), on_count, "50");
    pending = 1;
    run_until_done(5000);
    if (strcmp(driver, "postgres") != 0) { /* C procs cannot join a libpq transaction */
        tx_step = 0;
        kn_db_begin(db, tx_proc_begun, NULL);
        pending = 1;
        run_until_done(5000);
        CHECK_EQ_INT(tx_step, 1);
        kn_db_query_sql(db, &GET_KR, KN_DB_ARGS("c01"), on_count, "0");
        pending = 1;
        run_until_done(5000);
    }
}

/* ------------------------------------------------------------------ pub/sub */

static int notified;
static void on_notify(const char *channel, const char *payload, void *ud) {
    CHECK_EQ_STR(channel, "kn_test_chan");
    CHECK_EQ_STR(payload, "{\"lobby\":\"x\"}");
    notified++;
    pending--;
}

static void test_pubsub(kn_db *db) {
    CHECK(kn_db_listen(db, "kn_test_chan", on_notify, NULL) == 0);
    uint64_t end = kn_now_ms() + 700;
    while (kn_now_ms() < end) kn_loop_run_once(loop, 20);
    kn_db_notify(db, "kn_test_chan", "{\"lobby\":\"x\"}");
    kn_db_notify(db, "other_chan", "ignored");
    pending = 1;
    run_until_done(5000);
    end = kn_now_ms() + 400; /* no duplicate delivery */
    while (kn_now_ms() < end) kn_loop_run_once(loop, 20);
    CHECK_EQ_INT(notified, 1);
}

/* ------------------------------------------------------------------ sync conn + migrations */

static void write_file(const char *dir, const char *name, const char *text) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

static void test_migrate(const kn_db_config *dbc) {
    char err[1024], dir[256], sub[400];
    kn_dbconn *c = kn_dbconn_open(dbc, err, sizeof err);
    CHECK(c != NULL);
    if (!c) return;
    CHECK_EQ_STR(kn_dbconn_driver_name(c), driver);
    snprintf(dir, sizeof dir, "/tmp/kn-store-mig-XXXXXX");
    CHECK(mkdtemp(dir) != NULL);
    snprintf(sub, sizeof sub, "%s/%s", dir, kn_dbconn_dialect(c));
    CHECK(mkdir(sub, 0700) == 0);
    write_file(dir, "0001_wrong_dialect.sql", "THIS IS NOT RUN;");
    write_file(sub, "0001_a.sql", "CREATE TABLE mt (id INTEGER PRIMARY KEY, v VARCHAR(20));\n"
                                  "-- comment; with semicolon\nINSERT INTO mt (id, v) VALUES (1, 'a');\n");
    write_file(sub, "0002_b.sql", "INSERT INTO mt (id, v) VALUES (2, 'b');\n");
    kn_migrate_opts opts = {dir, 0, 0};
    CHECK_EQ_INT(kn_migrate_run(c, &opts, err, sizeof err), 2);
    CHECK_EQ_INT(kn_migrate_run(c, &opts, err, sizeof err), 0);
    kn_db_result *r = kn_dbconn_exec(c, "SELECT count(*) FROM mt", 0, NULL);
    CHECK_EQ_INT(kn_db_get_int64(r, 0, 0, -1), 2);
    kn_db_result_free(r);
    write_file(sub, "0003_bad.sql", "INSERT INTO mt (id, v) VALUES (3, 'c');\nINSERT INTO nope VALUES (1);\n");
    CHECK_EQ_INT(kn_migrate_run(c, &opts, err, sizeof err), -1);
    CHECK(strstr(err, "0003_bad.sql") != NULL);
    r = kn_dbconn_exec(c, "SELECT count(*) FROM mt", 0, NULL);
    CHECK_EQ_INT(kn_db_get_int64(r, 0, 0, -1), 2); /* the failed file left nothing behind */
    kn_db_result_free(r);
    r = kn_dbconn_exec(c, "SELECT count(*) FROM kn_migrations", 0, NULL);
    CHECK_EQ_INT(kn_db_get_int64(r, 0, 0, -1), 2);
    kn_db_result_free(r);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
    kn_dbconn_close(c);
}

/* ------------------------------------------------------------------ free with queued work */

static int closed_cbs, done_cbs;
static void on_closed(kn_db_result *r, void *ud) {
    if (!kn_db_ok(r) && kn_db_errclass_of(r) == KN_DB_E_CONNECTION) closed_cbs++;
    done_cbs++;
}

int main(int argc, char **argv) {
    kn_log_init("test");
    driver = argc > 1 ? argv[1] : "sqlite";
    kn_db_config dbc;
    int rc = setup(&dbc);
    if (rc) return rc;
    char err[512];
    kn_dbconn *c = kn_dbconn_open(&dbc, err, sizeof err);
    if (!c) {
        fprintf(stderr, "connect: %s\n", err);
        return 1;
    }
    kn_db_result *r = kn_dbconn_exec_sql(c, &DDL, 0, NULL);
    CHECK(kn_db_ok(r));
    if (!kn_db_ok(r)) fprintf(stderr, "DDL: %s\n", kn_db_error(r));
    kn_db_result_free(r);
    kn_dbconn_close(c);

    loop = kn_loop_new();
    kn_db *db = kn_db_new(loop, &dbc);
    CHECK(db != NULL);
    CHECK_EQ_STR(kn_db_driver_name(db), driver);
    CHECK(kn_db_wait_ready(db, 5000) == 0);
    test_basic(db);
    test_concurrency(db);
    test_prepared(db);
    test_transactions(db);
    test_procs(db);
    test_pubsub(db);
    test_migrate(&dbc);

    for (int i = 0; i < 40; i++) kn_db_query(db, "SELECT count(*) FROM accounts", 0, NULL, on_closed, NULL);
    kn_db_free(db);
    CHECK_EQ_INT(done_cbs, 40);  /* every callback ran */
    CHECK(closed_cbs > 0);       /* queued ones failed with 08003 */
    kn_loop_free(loop);
    teardown(&dbc);
    kn_config_free(g_cfg);
    printf("driver %s\n", driver);
    TEST_DONE("knsvc_store");
}
