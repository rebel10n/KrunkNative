/* knsvc_db pool against a real PostgreSQL (isolated database + schema). */
#include "db_test_util.h"
#include "kn_test.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#include <unistd.h>

/* Per-process names: concurrent runs (parallel agents, CI) share the krunknative_svccore database,
 * and test_reconnect terminates every backend with our application_name, so fixed names let one
 * run drop another's schema, kill its pool and steal its NOTIFYs. */
static char g_schema[48], g_app[48], g_chan[48];

static kn_loop *loop;
static int pending;

static void run_until_done(int timeout_ms) {
    uint64_t end = kn_now_ms() + (uint64_t)timeout_ms;
    while (pending > 0 && kn_now_ms() < end) kn_loop_run_once(loop, 50);
    CHECK_EQ_INT(pending, 0);
}

/* ---- basic queries */
static void on_insert(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_INT(kn_db_affected(r), 1);
    CHECK_EQ_INT(kn_db_rows(r), 1); /* RETURNING id */
    CHECK(kn_db_get_int64(r, 0, 0, -1) > 0);
    pending--;
}

static void on_select(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_INT(kn_db_rows(r), 2);
    CHECK_EQ_STR(kn_db_get_by(r, 0, "name"), "alice");
    CHECK_EQ_STR(kn_db_get_by(r, 1, "name"), "bob'); DROP TABLE accounts; --");
    int kr = kn_db_col(r, "kr");
    CHECK_EQ_INT(kn_db_get_int64(r, 0, kr, 0), 150);
    CHECK(kn_db_isnull(r, 1, kn_db_col(r, "bio")));
    CHECK_EQ_INT(kn_db_get_bool(r, 0, kn_db_col(r, "verified"), -1), 1);
    CHECK(kn_db_col(r, "nope") == -1 && kn_db_get_by(r, 0, "nope") == NULL);
    cJSON *j = kn_db_result_to_json(r);
    char *s = cJSON_PrintUnformatted(j);
    CHECK_EQ_STR(s, "[{\"id\":1,\"name\":\"alice\",\"kr\":150,\"verified\":true,\"bio\":\"hi\",\"stats\":{\"kills\":3},\"ratio\":0.5},"
                    "{\"id\":2,\"name\":\"bob'); DROP TABLE accounts; --\",\"kr\":0,\"verified\":false,\"bio\":null,\"stats\":null,\"ratio\":null}]");
    free(s);
    cJSON_Delete(j);
    pending--;
}

static void on_dup(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    CHECK(kn_db_is_unique_violation(r));
    CHECK_EQ_STR(kn_db_sqlstate(r), "23505");
    CHECK(strstr(kn_db_error(r), "duplicate key") != NULL);
    pending--;
}

static void on_syntax(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    CHECK_EQ_STR(kn_db_sqlstate(r), "42601");
    pending--;
}

static void test_basic(kn_db *db) {
    kn_db_query(db, "INSERT INTO accounts (name, kr, verified, bio, stats, ratio) VALUES ($1, $2, $3, $4, $5, $6) RETURNING id",
                KN_DB_ARGS("alice", "150", "true", "hi", "{\"kills\":3}", "0.5"), on_insert, NULL);
    pending = 1;
    run_until_done(5000);
    /* one at a time: on two pool connections bob could commit first and take id 1 (on_select
     * expects alice = 1, bob = 2) */
    kn_db_query(db, "INSERT INTO accounts (name) VALUES ($1) RETURNING id",
                KN_DB_ARGS("bob'); DROP TABLE accounts; --"), on_insert, NULL);
    pending = 1;
    run_until_done(5000);
    /* pool queries are unordered: the reads start once the inserts completed */
    kn_db_query(db, "SELECT * FROM accounts ORDER BY id", 0, NULL, on_select, NULL);
    kn_db_query(db, "INSERT INTO accounts (name) VALUES ($1)", KN_DB_ARGS("alice"), on_dup, NULL);
    kn_db_query(db, "SELEC 1", 0, NULL, on_syntax, NULL);
    pending = 3;
    run_until_done(5000);
}

/* ---- concurrency: 40 x pg_sleep(0.05) on 4 connections */
static int conc_done;
static void on_sleep(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    conc_done++;
    pending--;
}

static void test_concurrency(kn_db *db) {
    uint64_t t0 = kn_now_ms();
    for (int i = 0; i < 40; i++) kn_db_query(db, "SELECT pg_sleep(0.05)", 0, NULL, on_sleep, NULL);
    CHECK(kn_db_queue_len(db) > 0);
    pending = 40;
    run_until_done(10000);
    uint64_t dt = kn_now_ms() - t0;
    CHECK_EQ_INT(conc_done, 40);
    CHECK(dt < 40 * 50); /* ran in parallel */
    printf("40 x 50ms queries on 4 connections: %llu ms\n", (unsigned long long)dt);
}

/* ---- prepared statements */
static void on_prepared(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_INT(kn_db_rows(r), 1);
    CHECK_EQ_STR(kn_db_get(r, 0, 0), (const char *)ud);
    pending--;
}

static void on_bad_prepared(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    pending--;
}

static void test_prepared(kn_db *db) {
    CHECK(kn_db_prepare(db, "acct_by_name", "SELECT name FROM accounts WHERE name = $1") == 0);
    CHECK(kn_db_prepare(db, "acct_by_name", "SELECT 1") != 0);
    CHECK(kn_db_prepare(db, "broken", "SELECT FROM nowhere WHERE") == 0);
    for (int i = 0; i < 12; i++) kn_db_exec_prepared(db, "acct_by_name", KN_DB_ARGS("alice"), on_prepared, "alice");
    kn_db_exec_prepared(db, "broken", 0, NULL, on_bad_prepared, NULL);
    kn_db_exec_prepared(db, "unknown_stmt", 0, NULL, on_bad_prepared, NULL);
    pending = 14;
    run_until_done(5000);
}

/* ---- transactions */
static int tx_step;
static void tx_count_cb(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    CHECK_EQ_STR(kn_db_get(r, 0, 0), (const char *)ud);
    pending--;
}

static void tx_end_cb(kn_db_result *r, void *ud) {
    int expect_ok = ud != NULL;
    CHECK_EQ_INT(kn_db_ok(r), expect_ok);
    pending--;
}

static void tx_ins_cb(kn_db_result *r, void *ud) {
    CHECK(kn_db_ok(r));
    tx_step++;
}

static void tx_fail_cb(kn_db_result *r, void *ud) {
    CHECK(!kn_db_ok(r));
    tx_step++;
}

static void tx_commit_begun(kn_db_tx *tx, kn_db_result *r, void *ud) {
    CHECK(tx != NULL && kn_db_ok(r));
    kn_db_tx_query(tx, "INSERT INTO accounts (name) VALUES ($1)", KN_DB_ARGS("tx-commit"), tx_ins_cb, NULL);
    kn_db_tx_query(tx, "SELECT count(*) FROM accounts WHERE name = 'tx-commit'", 0, NULL, tx_count_cb, "1");
    kn_db_tx_commit(tx, tx_end_cb, (void *)1);
}

static void tx_rollback_begun(kn_db_tx *tx, kn_db_result *r, void *ud) {
    CHECK(tx != NULL);
    kn_db_tx_query(tx, "INSERT INTO accounts (name) VALUES ($1)", KN_DB_ARGS("tx-rollback"), tx_ins_cb, NULL);
    kn_db_tx_rollback(tx, tx_end_cb, (void *)1);
}

static void tx_failing_begun(kn_db_tx *tx, kn_db_result *r, void *ud) {
    CHECK(tx != NULL);
    kn_db_tx_query(tx, "INSERT INTO accounts (name) VALUES ($1)", KN_DB_ARGS("tx-failed"), tx_ins_cb, NULL);
    kn_db_tx_query(tx, "INSERT INTO accounts (name) VALUES ($1)", KN_DB_ARGS("alice"), tx_fail_cb, NULL); /* dup */
    kn_db_tx_commit(tx, tx_end_cb, NULL);  /* PostgreSQL turns COMMIT of a failed tx into ROLLBACK */
}

static void test_transactions(kn_db *db) {
    kn_db_begin(db, tx_commit_begun, NULL);
    kn_db_begin(db, tx_rollback_begun, NULL);
    kn_db_begin(db, tx_failing_begun, NULL);
    pending = 4; /* commit count + 2 ok ends + 1 failed end (the failing commit reports ok=?) */
    run_until_done(5000);
    kn_db_query(db, "SELECT count(*) FROM accounts WHERE name IN ('tx-rollback', 'tx-failed')", 0, NULL, tx_count_cb, "0");
    kn_db_query(db, "SELECT count(*) FROM accounts WHERE name = 'tx-commit'", 0, NULL, tx_count_cb, "1");
    pending = 2;
    run_until_done(5000);
    CHECK_EQ_INT(tx_step, 4);
}

/* ---- LISTEN/NOTIFY */
static int notified;
static void on_notify(const char *channel, const char *payload, void *ud) {
    CHECK_EQ_STR(channel, g_chan);
    CHECK_EQ_STR(payload, "{\"lobby\":\"x\"}");
    notified++;
    pending--;
}

static void test_notify(kn_db *db) {
    CHECK(kn_db_listen(db, g_chan, on_notify, NULL) == 0);
    /* give the listener a moment to connect + LISTEN */
    uint64_t end = kn_now_ms() + 1000;
    while (kn_now_ms() < end) kn_loop_run_once(loop, 20);
    kn_db_notify(db, g_chan, "{\"lobby\":\"x\"}");
    kn_db_notify(db, "other_chan", "ignored");
    pending = 1;
    run_until_done(5000);
    CHECK_EQ_INT(notified, 1);
}

/* ---- reconnect after the server kills our backends */
static void on_kill(kn_db_result *r, void *ud) {
    pending--;
}

static void on_after_kill(kn_db_result *r, void *ud) {
    if (kn_db_ok(r)) (*(int *)ud)++;
    pending--;
}

static void test_reconnect(kn_db *db, kn_dbconn *admin) {
    (void)db;
    char sql[256];
    snprintf(sql, sizeof sql,
             "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity WHERE application_name = '%s' AND pid <> pg_backend_pid()",
             g_app);
    kn_db_result *r = kn_dbconn_exec(admin, sql, 0, NULL);
    CHECK(kn_db_ok(r));
    printf("terminated %s pool backends\n", kn_db_get(r, 0, 0));
    kn_db_result_free(r);
    usleep(100000);
    int ok = 0;
    /* the first queries may fail (dead socket noticed on use); later ones must succeed after reconnect */
    for (int i = 0; i < 8; i++) kn_db_query(db, "SELECT 1", 0, NULL, on_after_kill, &ok);
    pending = 8;
    run_until_done(15000);
    uint64_t end = kn_now_ms() + 3000;
    while (kn_db_ready_count(db) < 4 && kn_now_ms() < end) kn_loop_run_once(loop, 50);
    ok = 0;
    for (int i = 0; i < 8; i++) kn_db_query(db, "SELECT 1", 0, NULL, on_after_kill, &ok);
    pending = 8;
    run_until_done(5000);
    CHECK_EQ_INT(ok, 8);
    CHECK_EQ_INT(kn_db_ready_count(db), 4);
    /* NOTIFY still works after the listener reconnected */
    end = kn_now_ms() + 1500;
    while (kn_now_ms() < end) kn_loop_run_once(loop, 20);
    kn_db_notify(db, g_chan, "{\"lobby\":\"x\"}");
    pending = 1;
    run_until_done(5000);
    CHECK_EQ_INT(notified, 2);
    (void)on_kill;
}

/* ---- free with queued work */
static int closed_cbs;
static void on_closed(kn_db_result *r, void *ud) {
    if (!kn_db_ok(r)) {
        CHECK_EQ_STR(kn_db_sqlstate(r), "08003");
        closed_cbs++;
    }
}

int main(void) {
    kn_log_init("test");
    kn_db_config dbc;
    kn_config *cfg = test_db_config(&dbc);
    kn_dbconn *admin = test_db_connect_or_skip(&dbc);
    snprintf(g_schema, sizeof g_schema, "kn_svccore_test_%d", (int)getpid());
    snprintf(g_app, sizeof g_app, "kn_svccore_test_%d", (int)getpid());
    snprintf(g_chan, sizeof g_chan, "kn_svccore_chan_%d", (int)getpid());
    char sql[512];
    snprintf(sql, sizeof sql,
             "DROP SCHEMA IF EXISTS %s CASCADE; CREATE SCHEMA %s;"
             "CREATE TABLE %s.accounts (id serial PRIMARY KEY, name text UNIQUE NOT NULL,"
             " kr bigint NOT NULL DEFAULT 0, verified boolean NOT NULL DEFAULT false, bio text,"
             " stats jsonb, ratio double precision)",
             g_schema, g_schema, g_schema);
    test_exec(admin, sql);
    loop = kn_loop_new();
    dbc.search_path = g_schema;
    dbc.pool_size = 4;
    dbc.application_name = g_app;
    kn_db *db = kn_db_new(loop, &dbc);
    CHECK(kn_db_wait_ready(db, 5000) == 0);
    test_basic(db);
    test_concurrency(db);
    test_prepared(db);
    test_transactions(db);
    test_notify(db);
    test_reconnect(db, admin);

    for (int i = 0; i < 5; i++) kn_db_query(db, "SELECT pg_sleep(1)", 0, NULL, on_closed, NULL);
    kn_loop_run_once(loop, 10);
    kn_db_free(db);
    CHECK_EQ_INT(closed_cbs, 5);

    snprintf(sql, sizeof sql, "DROP SCHEMA %s CASCADE", g_schema);
    test_exec(admin, sql);
    kn_dbconn_close(admin);
    kn_loop_free(loop);
    kn_config_free(cfg);
    TEST_DONE("knsvc_db");
}
