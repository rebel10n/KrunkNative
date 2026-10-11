/* Shared setup for the DB tests: credentials from config/config.json, database
 * KN_TEST_DATABASE (default krunknative_svccore, created with
 * `sudo -u postgres createdb -O krunknative krunknative_svccore`). Exit 77 = skip. */
#ifndef KN_DB_TEST_UTIL_H
#define KN_DB_TEST_UTIL_H

#include "knsvc/config.h"
#include "knsvc/db.h"

#include <stdio.h>
#include <stdlib.h>

static kn_config *test_db_config(kn_db_config *dbc) {
    char path[4096], err[256];
    snprintf(path, sizeof path, "%s/config/config.json", KN_REPO_ROOT);
    kn_config *cfg = kn_config_load(path, 0, NULL, err, sizeof err);
    if (!cfg) cfg = kn_config_from_string("{}", err, sizeof err);
    const char *name = getenv("KN_TEST_DATABASE");
    kn_config_set(cfg, "database.name", name && *name ? name : "krunknative_svccore");
    kn_db_config_from(cfg, dbc);
    return cfg;
}

/* Sync connection or exit(77). */
static kn_dbconn *test_db_connect_or_skip(const kn_db_config *dbc) {
    char err[512];
    kn_dbconn *c = kn_dbconn_open(dbc, err, sizeof err);
    if (!c) {
        printf("SKIP: cannot connect to test database %s: %s\n", dbc->name, err);
        exit(77);
    }
    return c;
}

static void test_exec(kn_dbconn *c, const char *sql) {
    char err[512];
    if (kn_dbconn_exec_script(c, sql, err, sizeof err) != 0) {
        fprintf(stderr, "setup failed: %s: %s\n", sql, err);
        exit(1);
    }
}

#endif
