/* Private pieces shared by db.c, db_result.c and dbconn.c. */
#ifndef KNSVC_DB_INTERNAL_H
#define KNSVC_DB_INTERNAL_H

#include <libpq-fe.h>

#include "knsvc/db.h"

struct kn_db_result {
    PGresult *pg;        /* NULL for synthetic errors */
    int ok;
    char sqlstate[6];
    char error[512];
};

/* Wrap a PGresult (takes ownership; may be NULL with conn for the error text). */
kn_db_result *kn_db__result_from_pg(PGresult *pg, PGconn *conn);
/* Synthetic error result. */
kn_db_result *kn_db__result_error(const char *sqlstate, const char *msg);
/* libpq keyword/value arrays for cfg (static storage inside out struct). */
typedef struct kn_db__conninfo {
    const char *keys[12];
    const char *vals[12];
    char port[16], timeout[16], options[512];
} kn_db__conninfo;
void kn_db__conninfo_build(const kn_db_config *cfg, kn_db__conninfo *ci);

#endif
