/*
 * Built-in database drivers (knsvc/store.h). Adding an adapter:
 *   1. src/drivers/store_<name>.c defining `const kn_store_driver kn_store_driver_<name>`;
 *   2. one knsvc_store_driver(<OPTION> <name> <source> <libs>) line in
 *      services/common/CMakeLists.txt (+ its option/find_package in
 *      services/CMakeLists.txt): it lists the driver in the generated
 *      knsvc_store_drivers.h included below.
 * doc/backend.md 3.6 "Writing a new adapter".
 */
#include "store_internal.h"

#include <string.h>

#define KN_STORE_DRIVER(n) extern const kn_store_driver kn_store_driver_##n;
#include "knsvc_store_drivers.h"
#undef KN_STORE_DRIVER

#define KN_STORE_DRIVER(n) &kn_store_driver_##n,
static const kn_store_driver *const k_builtin[] = {
#include "knsvc_store_drivers.h"
    NULL};
#undef KN_STORE_DRIVER

#define MAX_EXTRA 8
static const kn_store_driver *g_extra[MAX_EXTRA];
static size_t g_nextra;

static size_t builtin_count(void) { return sizeof k_builtin / sizeof *k_builtin - 1; }

size_t kn_store_driver_count(void) { return builtin_count() + g_nextra; }

const kn_store_driver *kn_store_driver_at(size_t i) {
    if (i < builtin_count()) return k_builtin[i];
    i -= builtin_count();
    return i < g_nextra ? g_extra[i] : NULL;
}

const kn_store_driver *kn_store_driver_find(const char *name) {
    if (!name) return NULL;
    /* aliases */
    if (!strcmp(name, "postgresql") || !strcmp(name, "pg")) name = "postgres";
    if (!strcmp(name, "mariadb")) name = "mysql";
    if (!strcmp(name, "sqlite3")) name = "sqlite";
    for (size_t i = 0; i < kn_store_driver_count(); i++)
        if (!strcmp(kn_store_driver_at(i)->name, name)) return kn_store_driver_at(i);
    return NULL;
}

int kn_store_driver_register(const kn_store_driver *drv) {
    if (!drv || !drv->name || !drv->blocking || g_nextra >= MAX_EXTRA || kn_store_driver_find(drv->name)) return -1;
    g_extra[g_nextra++] = drv;
    return 0;
}

/* database.driver absent: PostgreSQL when a server is configured, else the
 * zero-config SQLite file; whatever is built in otherwise. */
const kn_store_driver *kn_store_default_driver(int network_configured) {
    const kn_store_driver *pg = kn_store_driver_find("postgres");
    const kn_store_driver *lite = kn_store_driver_find("sqlite");
    if (network_configured && pg) return pg;
    if (lite) return lite;
    if (pg) return pg;
    return kn_store_driver_count() ? kn_store_driver_at(0) : NULL;
}

#ifndef KNSVC_STORE_HAVE_POSTGRES
void *kn_db_result_pg(const kn_db_result *res) { (void)res; return NULL; }
void *kn_dbconn_pg(kn_dbconn *c) { (void)c; return NULL; }
#endif
