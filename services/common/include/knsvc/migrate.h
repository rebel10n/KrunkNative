/*
 * knsvc/migrate.h - SQL migrations runner (library `knsvc_db`).
 *
 * Files: <dir>/NNNN_name.sql (NNNN = 4+ digits), applied in numeric order.
 * Each file runs inside its own transaction together with its bookkeeping row
 * in kn_migrations(version, name, checksum sha256 hex, applied_at), so a
 * failing file leaves nothing behind. A session advisory lock serialises
 * concurrent runners (several brains starting at once). Already applied files
 * whose checksum changed are reported as warnings (never re-run).
 * Files must not contain BEGIN/COMMIT themselves.
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
/* Whole CLI: connect with cfg's database section, run (or --status/--dry-run
 * from argv), log the outcome. Returns a process exit code. */
int kn_migrate_main(const struct kn_config *cfg, int argc, char **argv);

#endif
