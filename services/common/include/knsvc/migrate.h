/*
 * knsvc/migrate.h - SQL migrations runner (library `knsvc_db`).
 *
 * Files: <dir>/<dialect>/NNNN_name.sql when that subdirectory exists
 * (services/db/migrations/{postgres,sqlite,mysql}), else <dir>/NNNN_name.sql;
 * NNNN = 4+ digits, applied in numeric order. Each file runs inside its own
 * transaction together with its bookkeeping row in kn_migrations(version, name,
 * checksum sha256 hex, applied_at), so a failing file leaves nothing behind
 * (MySQL: DDL commits implicitly, only the bookkeeping row is atomic). A session
 * lock serialises concurrent runners (postgres advisory lock, mysql GET_LOCK,
 * sqlite BEGIN IMMEDIATE + re-check). Already applied files whose checksum
 * changed are reported as warnings (never re-run). Files must not contain
 * BEGIN/COMMIT themselves. A new migration gets the same number in every dialect.
 *
 * Version ranges: 0001-0099 core, 0100-0199 accounts/social, 0200-0299
 * matchmaker/registry, 0300-0399 game servers / stats.
 *
 * Entry points: `kn_migrate` tool, `kn_brain --migrate`, `make db-migrate`.
 */
#ifndef KNSVC_MIGRATE_H
#define KNSVC_MIGRATE_H

#include <stddef.h>

#include "knsvc/db.h"

struct kn_config;

typedef struct kn_migrate_opts {
    const char *dir;      /* default: config "database.migrations_dir" or services/db/migrations */
    int dry_run;          /* list pending, apply nothing */
    int verbose;
} kn_migrate_opts;

/* Apply pending migrations over a synchronous connection. Returns the number
 * applied (>= 0) or -1 with a message in err. */
int kn_migrate_run(kn_dbconn *conn, const kn_migrate_opts *opts, char *err, size_t errlen);
/* Print applied/pending migrations to stdout. 0 ok, -1 error. */
int kn_migrate_status(kn_dbconn *conn, const char *dir, char *err, size_t errlen);
/* Migrations directory: database.migrations_dir (resolved against the config
 * root) when set, else the first existing of <config root>/services/db/migrations
 * and the same path next to / above the running executable (release layouts).
 * kn_migrate_run then picks its dialect subdirectory. Returns buf, or NULL. */
char *kn_migrate_resolve_dir(const struct kn_config *cfg, char *buf, size_t cap);
/* Whole CLI: connect with cfg's database section, run (or --status/--dry-run
 * from argv), log the outcome. Returns a process exit code. */
int kn_migrate_main(const struct kn_config *cfg, int argc, char **argv);

#endif
