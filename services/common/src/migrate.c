/* knsvc/migrate.h: ordered, transactional, advisory-locked SQL migrations. */
#include "knsvc/migrate.h"
#include "knsvc/buf.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIGRATE_LOCK_KEY "7020896536452359" /* arbitrary constant shared by every runner */
#define DEFAULT_DIR "services/db/migrations"

typedef struct mfile {
    long long version;
    char *name;   /* file name */
    char *path;
} mfile;

typedef struct applied {
    long long version;
    char name[256];
    char checksum[65];
} applied;

static void set_err(char *err, size_t n, const char *fmt, const char *a, const char *b) {
    if (err && n) snprintf(err, n, fmt, a ? a : "", b ? b : "");
}

/* "0012_name.sql" -> 12; -1 when the name does not match NNNN_*.sql */
static long long file_version(const char *name) {
    size_t digits = strspn(name, "0123456789");
    size_t len = strlen(name);
    if (digits < 4 || name[digits] != '_' || len < digits + 6 || strcmp(name + len - 4, ".sql") != 0) return -1;
    return strtoll(name, NULL, 10);
}

static int cmp_mfile(const void *a, const void *b) {
    const mfile *x = a, *y = b;
    return x->version < y->version ? -1 : x->version > y->version;
}

static void free_files(mfile *f, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(f[i].name);
        free(f[i].path);
    }
    free(f);
}

static int list_files(const char *dir, mfile **out, size_t *count, char *err, size_t errlen) {
    DIR *d = opendir(dir);
    *out = NULL;
    *count = 0;
    if (!d) {
        set_err(err, errlen, "cannot open migrations dir %s: %s", dir, strerror(errno));
        return -1;
    }
    mfile *files = NULL;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        long long v = file_version(e->d_name);
        if (v < 0) continue;
        mfile *nf = realloc(files, (n + 1) * sizeof *nf);
        if (!nf) break;
        files = nf;
        files[n].version = v;
        files[n].name = strdup(e->d_name);
        size_t pl = strlen(dir) + strlen(e->d_name) + 2;
        files[n].path = malloc(pl);
        if (files[n].path) snprintf(files[n].path, pl, "%s/%s", dir, e->d_name);
        n++;
    }
    closedir(d);
    qsort(files, n, sizeof *files, cmp_mfile);
    for (size_t i = 1; i < n; i++) {
        if (files[i].version == files[i - 1].version) {
            set_err(err, errlen, "duplicate migration version: %s and %s", files[i - 1].name, files[i].name);
            free_files(files, n);
            return -1;
        }
    }
    *out = files;
    *count = n;
    return 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    kn_buf b = {0};
    char tmp[8192];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) kn_buf_append(&b, tmp, r);
    fclose(f);
    if (!b.data) kn_buf_reserve(&b, 0);
    return kn_buf_detach(&b, len);
}

static void sha256_hex(const char *data, size_t len, char out[65]) {
    uint8_t d[KN_SHA256_LEN];
    kn_sha256(data, len, d);
    kn_hex_encode(d, sizeof d, out, 65);
}

static int exec_ok(kn_dbconn *c, const char *sql, int n, const char *const *params, char *err, size_t errlen) {
    kn_db_result *r = kn_dbconn_exec(c, sql, n, params);
    int ok = kn_db_ok(r);
    if (!ok) set_err(err, errlen, "%s%s", kn_db_error(r), NULL);
    kn_db_result_free(r);
    return ok ? 0 : -1;
}

static int ensure_table(kn_dbconn *c, char *err, size_t errlen) {
    return exec_ok(c,
                   "CREATE TABLE IF NOT EXISTS kn_migrations ("
                   " version bigint PRIMARY KEY,"
                   " name text NOT NULL,"
                   " checksum text NOT NULL,"
                   " applied_at timestamptz NOT NULL DEFAULT now())",
                   0, NULL, err, errlen);
}

static int load_applied(kn_dbconn *c, applied **out, size_t *count, char *err, size_t errlen) {
    kn_db_result *r = kn_dbconn_exec(c, "SELECT version, name, checksum FROM kn_migrations ORDER BY version", 0, NULL);
    *out = NULL;
    *count = 0;
    if (!kn_db_ok(r)) {
        set_err(err, errlen, "%s%s", kn_db_error(r), NULL);
        kn_db_result_free(r);
        return -1;
    }
    int rows = kn_db_rows(r);
    applied *a = calloc(rows ? (size_t)rows : 1, sizeof *a);
    for (int i = 0; a && i < rows; i++) {
        a[i].version = kn_db_get_int64(r, i, 0, -1);
        snprintf(a[i].name, sizeof a[i].name, "%s", kn_db_get(r, i, 1));
        snprintf(a[i].checksum, sizeof a[i].checksum, "%s", kn_db_get(r, i, 2));
    }
    kn_db_result_free(r);
    if (!a) return -1;
    *out = a;
    *count = (size_t)rows;
    return 0;
}

static const applied *find_applied(const applied *a, size_t n, long long v) {
    for (size_t i = 0; i < n; i++)
        if (a[i].version == v) return &a[i];
    return NULL;
}

static int lock(kn_dbconn *c, int on) {
    kn_db_result *r = kn_dbconn_exec(c, on ? "SELECT pg_advisory_lock(" MIGRATE_LOCK_KEY ")"
                                           : "SELECT pg_advisory_unlock(" MIGRATE_LOCK_KEY ")", 0, NULL);
    int ok = kn_db_ok(r);
    kn_db_result_free(r);
    return ok ? 0 : -1;
}

/* Apply one file in its own transaction. */
static int apply_one(kn_dbconn *c, const mfile *f, const char *sql, const char *sum, char *err, size_t errlen) {
    char e[1024] = "";
    if (kn_dbconn_exec_script(c, "BEGIN", e, sizeof e) != 0) goto fail;
    if (kn_dbconn_exec_script(c, sql, e, sizeof e) != 0) goto rollback;
    char ver[32];
    snprintf(ver, sizeof ver, "%lld", f->version);
    if (exec_ok(c, "INSERT INTO kn_migrations (version, name, checksum) VALUES ($1, $2, $3)",
                KN_DB_ARGS(ver, f->name, sum), e, sizeof e) != 0)
        goto rollback;
    if (kn_dbconn_exec_script(c, "COMMIT", e, sizeof e) != 0) goto fail;
    return 0;
rollback:
    kn_dbconn_exec_script(c, "ROLLBACK", NULL, 0);
fail:
    if (err && errlen) snprintf(err, errlen, "%s: %s", f->name, e);
    return -1;
}

int kn_migrate_run(kn_dbconn *c, const kn_migrate_opts *opts, char *err, size_t errlen) {
    const char *dir = opts && opts->dir ? opts->dir : DEFAULT_DIR;
    mfile *files;
    size_t nfiles;
    if (list_files(dir, &files, &nfiles, err, errlen) != 0) return -1;
    if (lock(c, 1) != 0) {
        set_err(err, errlen, "cannot take the migrations lock%s%s", NULL, NULL);
        free_files(files, nfiles);
        return -1;
    }
    applied *done = NULL;
    size_t ndone = 0;
    int count = 0;
    if (ensure_table(c, err, errlen) != 0 || load_applied(c, &done, &ndone, err, errlen) != 0) {
        count = -1;
        goto out;
    }
    for (size_t i = 0; i < nfiles; i++) {
        size_t len = 0;
        char *sql = read_file(files[i].path, &len);
        if (!sql) {
            set_err(err, errlen, "cannot read %s: %s", files[i].path, strerror(errno));
            count = -1;
            break;
        }
        char sum[65];
        sha256_hex(sql, len, sum);
        const applied *a = find_applied(done, ndone, files[i].version);
        if (a) {
            if (strcmp(a->checksum, sum) != 0)
                KN_WARN("migrate", "%s changed after it was applied (checksum mismatch); not re-run", files[i].name);
            free(sql);
            continue;
        }
        if (opts && opts->dry_run) {
            KN_INFO("migrate", "pending: %s", files[i].name);
            count++;
            free(sql);
            continue;
        }
        if (opts && opts->verbose) KN_INFO("migrate", "applying %s", files[i].name);
        int rc = apply_one(c, &files[i], sql, sum, err, errlen);
        free(sql);
        if (rc != 0) {
            count = -1;
            break;
        }
        count++;
    }
out:
    lock(c, 0);
    free(done);
    free_files(files, nfiles);
    return count;
}

int kn_migrate_status(kn_dbconn *c, const char *dir, char *err, size_t errlen) {
    if (!dir) dir = DEFAULT_DIR;
    mfile *files;
    size_t nfiles;
    if (list_files(dir, &files, &nfiles, err, errlen) != 0) return -1;
    applied *done = NULL;
    size_t ndone = 0;
    if (ensure_table(c, err, errlen) != 0 || load_applied(c, &done, &ndone, err, errlen) != 0) {
        free_files(files, nfiles);
        return -1;
    }
    for (size_t i = 0; i < nfiles; i++) {
        const applied *a = find_applied(done, ndone, files[i].version);
        printf("%-8s %s\n", a ? "applied" : "pending", files[i].name);
    }
    for (size_t i = 0; i < ndone; i++) {
        int local = 0;
        for (size_t k = 0; k < nfiles; k++) local |= files[k].version == done[i].version;
        if (!local) printf("%-8s %s (no local file)\n", "applied", done[i].name);
    }
    free(done);
    free_files(files, nfiles);
    return 0;
}

int kn_migrate_main(const kn_config *cfg, int argc, char **argv) {
    kn_migrate_opts opts = {0};
    int status = 0;
    const char *dir_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (kn_config_arg_consumed(cfg, i)) continue;
        if (!strcmp(argv[i], "--status")) status = 1;
        else if (!strcmp(argv[i], "--dry-run")) opts.dry_run = 1;
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) opts.verbose = 1;
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir_arg = argv[++i];
        else if (!strncmp(argv[i], "--dir=", 6)) dir_arg = argv[i] + 6;
    }
    char dir[4096];
    const char *rel = dir_arg ? dir_arg : kn_config_str(cfg, "database.migrations_dir", DEFAULT_DIR);
    if (!kn_config_resolve_path(cfg, rel, dir, sizeof dir)) return 2;
    opts.dir = dir;
    opts.verbose = 1;

    kn_db_config dbc;
    kn_db_config_from(cfg, &dbc);
    char err[1024];
    kn_dbconn *c = kn_dbconn_open(&dbc, err, sizeof err);
    if (!c) {
        KN_ERROR("migrate", "cannot connect to %s@%s:%d/%s: %s", dbc.user ? dbc.user : "", dbc.host, dbc.port,
                 dbc.name ? dbc.name : "", err);
        return 1;
    }
    int rc;
    if (status) {
        rc = kn_migrate_status(c, dir, err, sizeof err) == 0 ? 0 : 1;
        if (rc) KN_ERROR("migrate", "%s", err);
    } else {
        int n = kn_migrate_run(c, &opts, err, sizeof err);
        if (n < 0) KN_ERROR("migrate", "failed: %s", err);
        else KN_INFO("migrate", "%s %d migration(s) from %s", opts.dry_run ? "pending" : "applied", n, dir);
        rc = n < 0 ? 1 : 0;
    }
    kn_dbconn_close(c);
    return rc;
}
