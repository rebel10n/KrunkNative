/*
 * kn_migrate - apply services/db/migrations/NNNN_*.sql to the configured database.
 *   kn_migrate [--config FILE] [--status] [--dry-run] [--dir DIR] [--set database.name=x]
 * (`make db-migrate`; `kn_brain --migrate` does the same through kn_migrate_main.)
 */
#include "knsvc/config.h"
#include "knsvc/log.h"
#include "knsvc/migrate.h"

#include <stdio.h>

int main(int argc, char **argv) {
    kn_log_init("migrate");
    char err[512];
    kn_config *cfg = kn_config_load(NULL, argc, argv, err, sizeof err);
    if (!cfg) {
        KN_ERROR("config", "%s", err);
        return 2;
    }
    kn_log_configure(cfg);
    int rc = kn_migrate_main(cfg, argc, argv);
    kn_config_free(cfg);
    return rc;
}
