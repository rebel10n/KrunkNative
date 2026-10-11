/* Migrations runner: ordering, idempotence, atomic failure, duplicates, dry run. */
#include "db_test_util.h"
#include "kn_test.h"
#include "knsvc/log.h"
#include "knsvc/migrate.h"

#include <sys/stat.h>
#include <unistd.h>

#define SCHEMA "kn_svccore_mig"

static char dir[256];

static void write_file(const char *name, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

static void remove_file(const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    unlink(p);
}

static long long scalar(kn_dbconn *c, const char *sql) {
    kn_db_result *r = kn_dbconn_exec(c, sql, 0, NULL);
    long long v = kn_db_ok(r) ? kn_db_get_int64(r, 0, 0, -1) : -1;
    kn_db_result_free(r);
    return v;
}

int main(void) {
    kn_log_init("test");
    kn_db_config dbc;
    kn_config *cfg = test_db_config(&dbc);
    kn_dbconn *admin = test_db_connect_or_skip(&dbc);
    test_exec(admin, "DROP SCHEMA IF EXISTS " SCHEMA " CASCADE; CREATE SCHEMA " SCHEMA);
    dbc.search_path = SCHEMA;
    char err[1024];
    kn_dbconn *c = kn_dbconn_open(&dbc, err, sizeof err);
    CHECK(c != NULL);

    snprintf(dir, sizeof dir, "/tmp/kn-svccore-mig-XXXXXX");
    CHECK(mkdtemp(dir) != NULL);
    write_file("0002_b.sql", "INSERT INTO t (v) VALUES ('from b');\nCREATE FUNCTION f() RETURNS int AS $$ SELECT 1; $$ LANGUAGE sql;\n");
    write_file("0001_a.sql", "CREATE TABLE t (id serial PRIMARY KEY, v text);\n-- comment; with semicolon\nINSERT INTO t (v) VALUES ('from a');\n");
    write_file("README.txt", "ignored");
    write_file("12_short.sql", "this is not a migration (needs 4 digits)");

    kn_migrate_opts dry = {dir, 1, 0};
    CHECK_EQ_INT(kn_migrate_run(c, &dry, err, sizeof err), 2);
    CHECK_EQ_INT(scalar(c, "SELECT count(*) FROM kn_migrations"), 0);

    kn_migrate_opts opts = {dir, 0, 0};
    CHECK_EQ_INT(kn_migrate_run(c, &opts, err, sizeof err), 2);
    CHECK_EQ_INT(scalar(c, "SELECT count(*) FROM t"), 2);
    CHECK_EQ_INT(scalar(c, "SELECT id FROM t WHERE v = 'from b'"), 2); /* a ran before b */
    CHECK_EQ_INT(scalar(c, "SELECT count(*) FROM kn_migrations"), 2);
    CHECK_EQ_INT(kn_migrate_run(c, &opts, err, sizeof err), 0);         /* idempotent */

    /* failing migration leaves nothing behind */
    write_file("0003_bad.sql", "CREATE TABLE u (id int);\nINSERT INTO nope VALUES (1);\n");
    CHECK_EQ_INT(kn_migrate_run(c, &opts, err, sizeof err), -1);
    CHECK(strstr(err, "0003_bad.sql") != NULL && strstr(err, "nope") != NULL);
    CHECK_EQ_INT(scalar(c, "SELECT count(*) FROM information_schema.tables WHERE table_schema = '" SCHEMA "' AND table_name = 'u'"), 0);
    CHECK_EQ_INT(scalar(c, "SELECT count(*) FROM kn_migrations"), 2);
    /* the connection is still usable after the failure */
    CHECK_EQ_INT(scalar(c, "SELECT 7"), 7);

    /* fixed file applies; a changed applied file only warns */
    write_file("0003_bad.sql", "CREATE TABLE u (id int);\n");
    write_file("0001_a.sql", "-- edited after apply\n");
    CHECK_EQ_INT(kn_migrate_run(c, &opts, err, sizeof err), 1);
    CHECK_EQ_INT(scalar(c, "SELECT count(*) FROM kn_migrations"), 3);

    CHECK_EQ_INT(kn_migrate_status(c, dir, err, sizeof err), 0);

    /* duplicate versions are rejected */
    write_file("0003_dup.sql", "SELECT 1;");
    CHECK_EQ_INT(kn_migrate_run(c, &opts, err, sizeof err), -1);
    CHECK(strstr(err, "duplicate") != NULL);
    remove_file("0003_dup.sql");

    /* missing directory */
    kn_migrate_opts nodir = {"/nonexistent/dir", 0, 0};
    CHECK_EQ_INT(kn_migrate_run(c, &nodir, err, sizeof err), -1);

    /* the real repository migrations apply cleanly into a fresh schema */
    test_exec(admin, "DROP SCHEMA IF EXISTS " SCHEMA "_repo CASCADE; CREATE SCHEMA " SCHEMA "_repo");
    kn_db_config repo_dbc = dbc;
    repo_dbc.search_path = SCHEMA "_repo";
    kn_dbconn *rc = kn_dbconn_open(&repo_dbc, err, sizeof err);
    char repo_dir[512];
    snprintf(repo_dir, sizeof repo_dir, "%s/services/db/migrations", KN_REPO_ROOT);
    kn_migrate_opts ropts = {repo_dir, 0, 0};
    int n = kn_migrate_run(rc, &ropts, err, sizeof err);
    if (n < 0) fprintf(stderr, "repository migrations failed: %s\n", err);
    CHECK(n >= 0);
    printf("repository migrations applied: %d\n", n);
    CHECK_EQ_INT(kn_migrate_run(rc, &ropts, err, sizeof err), 0);
    kn_dbconn_close(rc);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
    kn_dbconn_close(c);
    test_exec(admin, "DROP SCHEMA " SCHEMA " CASCADE; DROP SCHEMA " SCHEMA "_repo CASCADE");
    kn_dbconn_close(admin);
    kn_config_free(cfg);
    TEST_DONE("knsvc_migrate");
}
