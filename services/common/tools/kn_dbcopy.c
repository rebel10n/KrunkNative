/*
 * kn_dbcopy - copy a coordinator SQLite database into PostgreSQL and verify it
 * (doc/backend.md 3.11 "Moving a coordinator to PostgreSQL", deploy/ansible cutover).
 *
 *   kn_dbcopy --sqlite FILE [--config F] [--set database.x=y ...] [--replace] [--verify-only]
 *             [--report FILE]
 *
 * The destination is the config's database, which must be postgres and already migrated
 * (kn_migrate / kn_brain --migrate-only) to at least every version the SQLite file has.
 * Every SQLite table (but kn_migrations, kn_pubsub and sqlite_*) is copied in one
 * PostgreSQL transaction: the destination tables are TRUNCATEd first (so a re-run gives
 * the same result: idempotent), rows are inserted parents first (foreign keys from
 * pg_constraint), user triggers are disabled meanwhile (follows_count must not count the
 * copied follows twice), identity / serial sequences are set past the copied ids (or the
 * SQLite AUTOINCREMENT counter when that is higher). Values are converted by the
 * destination column type (booleans, UTC timestamps -> timestamptz, hex -> bytea, JSON
 * text -> jsonb, REAL read with 17 digits).
 *
 * Then both sides are verified per table: row count and a SHA-256 over the rows with
 * every value normalised by the destination type and the rows sorted, so a row that
 * differs anywhere changes the checksum. Exit 0 = copied and identical, 3 = verification
 * mismatch (the copy is committed: re-run, or fall back to SQLite), 1 = error (nothing
 * committed), 2 = usage. Refuses (1) when the destination already holds accounts and
 * --replace is not given. Never writes to the SQLite file (opened read-only).
 */
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

/* the schema had 140 tables in 0365; 128 here silently left the rest (trades onwards) uncopied. More than this is
 * refused (load_tables), never truncated. */
#define MAX_TABLES 512
#define MAX_COLS 64

typedef enum { T_TEXT, T_INT, T_FLOAT, T_BOOL, T_TS, T_DATE, T_JSON, T_BYTEA } ctype;

typedef struct Col {
    char name[64];
    char pgtype[64];
    ctype t;
    int src_real;      /* SQLite declared REAL/FLOAT/DOUBLE */
} Col;

typedef struct Table {
    char name[64];
    Col cols[MAX_COLS];
    int ncols;
    int identity;      /* has an identity column: INSERT ... OVERRIDING SYSTEM VALUE */
    int64_t src_rows, dst_rows;
    char src_sum[65], dst_sum[65];
    int done;          /* topological order */
} Table;

static Table g_t[MAX_TABLES];
static int g_n;
static FILE *g_report;

static void out(const char *fmt, ...) KN_PRINTF(1, 2);
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_report) {
        va_start(ap, fmt);
        vfprintf(g_report, fmt, ap);
        va_end(ap);
    }
    fflush(stdout);
}

static int fail(kn_db_result *r, const char *what) {
    fprintf(stderr, "kn_dbcopy: %s: %s\n", what, r ? kn_db_error(r) : "no result");
    if (r) kn_db_result_free(r);
    return -1;
}

static int exec_ok(kn_dbconn *c, const char *sql, const char *what) {
    kn_db_result *r = kn_dbconn_exec(c, sql, 0, NULL);
    if (!kn_db_ok(r)) return fail(r, what);
    kn_db_result_free(r);
    return 0;
}

static ctype map_type(const char *pg) {
    if (!strcmp(pg, "boolean")) return T_BOOL;
    if (!strcmp(pg, "smallint") || !strcmp(pg, "integer") || !strcmp(pg, "bigint")) return T_INT;
    if (!strcmp(pg, "numeric") || !strcmp(pg, "double precision") || !strcmp(pg, "real")) return T_FLOAT;
    if (!strncmp(pg, "timestamp", 9)) return T_TS;
    if (!strcmp(pg, "date")) return T_DATE;
    if (!strcmp(pg, "json") || !strcmp(pg, "jsonb")) return T_JSON;
    if (!strcmp(pg, "bytea")) return T_BYTEA;
    return T_TEXT;
}

static int is_ident(const char *s) {
    if (!*s || strlen(s) >= 64) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

/* ------------------------------------------------------------------ normalisation (checksums) */

static void canon_json(cJSON *j) {
    if (cJSON_IsObject(j)) {
        /* sort the children by key (insertion sort on the linked list) */
        cJSON *sorted = NULL;
        cJSON *it = j->child;
        while (it) {
            cJSON *next = it->next;
            it->prev = it->next = NULL;
            canon_json(it);
            if (!sorted || strcmp(it->string, sorted->string) < 0) {
                it->next = sorted;
                sorted = it;
            } else {
                cJSON *p = sorted;
                while (p->next && strcmp(p->next->string, it->string) <= 0) p = p->next;
                it->next = p->next;
                p->next = it;
            }
            it = next;
        }
        cJSON *prev = NULL;
        for (cJSON *p = sorted; p; p = p->next) {
            p->prev = prev;
            prev = p;
        }
        if (sorted) sorted->prev = prev; /* cJSON: child->prev is the last item */
        j->child = sorted;
    } else if (cJSON_IsArray(j)) {
        for (cJSON *it = j->child; it; it = it->next) canon_json(it);
    }
}

/* Appends the normal form of one value to buf. */
static void norm(char **buf, size_t *len, size_t *cap, const char *v, int isnull, ctype t) {
    char tmp[64];
    const char *s = v;
    char *owned = NULL;
    if (isnull) {
        s = "\\N";
    } else {
        switch (t) {
        case T_BOOL:
            s = (!strcmp(v, "1") || !strcmp(v, "t") || !strcmp(v, "true") || !strcmp(v, "TRUE")) ? "1" : "0";
            break;
        case T_INT:
            snprintf(tmp, sizeof tmp, "%lld", strtoll(v, NULL, 10));
            s = tmp;
            break;
        case T_FLOAT:
            snprintf(tmp, sizeof tmp, "%.17g", strtod(v, NULL));
            s = tmp;
            break;
        case T_TS: { /* -> YYYY-MM-DD HH:MM:SS.ffffff (UTC) */
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, n = 0;
            if (sscanf(v, "%d-%d-%d%*1[ T]%d:%d:%d%n", &y, &mo, &d, &h, &mi, &se, &n) >= 6) {
                char frac[8] = "000000";
                if (v[n] == '.') {
                    int k = 0;
                    for (const char *p = v + n + 1; isdigit((unsigned char)*p) && k < 6; p++) frac[k++] = *p;
                }
                snprintf(tmp, sizeof tmp, "%04d-%02d-%02d %02d:%02d:%02d.%s", y, mo, d, h, mi, se, frac);
                s = tmp;
            }
            break;
        }
        case T_JSON: {
            cJSON *j = cJSON_Parse(v);
            if (j) {
                canon_json(j);
                owned = cJSON_PrintUnformatted(j);
                cJSON_Delete(j);
                if (owned) s = owned;
            }
            break;
        }
        case T_BYTEA:
            if (v[0] == '\\' && v[1] == 'x') s = v + 2;
            owned = strdup(s);
            if (owned)
                for (char *p = owned; *p; p++) *p = (char)tolower((unsigned char)*p);
            s = owned ? owned : s;
            break;
        default:
            break;
        }
    }
    size_t n = strlen(s);
    if (*len + n + 2 > *cap) {
        *cap = (*len + n + 2) * 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[(*len)++] = '\x1f';
    (*buf)[*len] = 0;
    free(owned);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* SHA-256 over the sorted normalised rows of a result. */
static void checksum(kn_db_result *r, const Table *t, char out_hex[65], int64_t *rows) {
    int n = kn_db_rows(r);
    char **lines = calloc((size_t)(n ? n : 1), sizeof *lines);
    for (int i = 0; i < n; i++) {
        char *buf = NULL;
        size_t len = 0, cap = 0;
        for (int c = 0; c < t->ncols; c++)
            norm(&buf, &len, &cap, kn_db_get(r, i, c), kn_db_isnull(r, i, c), t->cols[c].t);
        lines[i] = buf ? buf : strdup("");
    }
    qsort(lines, (size_t)n, sizeof *lines, cmp_str);
    size_t total = 1;
    for (int i = 0; i < n; i++) total += strlen(lines[i]) + 1;
    char *all = malloc(total);
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        size_t k = strlen(lines[i]);
        memcpy(all + off, lines[i], k);
        off += k;
        all[off++] = '\x1e';
        free(lines[i]);
    }
    free(lines);
    unsigned char d[32];
    kn_sha256(all, off, d);
    kn_hex_encode(d, 32, out_hex, 65);
    free(all);
    *rows = n;
}

static char *src_select(const Table *t) {
    size_t cap = 256 + (size_t)t->ncols * 96;
    char *q = malloc(cap);
    size_t o = (size_t)snprintf(q, cap, "SELECT ");
    for (int c = 0; c < t->ncols; c++)
        o += (size_t)snprintf(q + o, cap - o, t->cols[c].src_real ? "%sprintf('%%!.17g', \"%s\")" : "%s\"%s\"",
                              c ? ", " : "", t->cols[c].name);
    snprintf(q + o, cap - o, " FROM \"%s\"", t->name);
    return q;
}

static char *dst_select(const Table *t) {
    size_t cap = 256 + (size_t)t->ncols * 128;
    char *q = malloc(cap);
    size_t o = (size_t)snprintf(q, cap, "SELECT ");
    for (int c = 0; c < t->ncols; c++) {
        const char *n = t->cols[c].name;
        const char *sep = c ? ", " : "";
        switch (t->cols[c].t) {
        case T_TS:
            o += (size_t)snprintf(q + o, cap - o, "%sto_char(\"%s\" AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS.US')", sep, n);
            break;
        case T_BYTEA: o += (size_t)snprintf(q + o, cap - o, "%sencode(\"%s\", 'hex')", sep, n); break;
        default: o += (size_t)snprintf(q + o, cap - o, "%s\"%s\"::text", sep, n); break;
        }
    }
    snprintf(q + o, cap - o, " FROM \"%s\"", t->name);
    return q;
}

static int verify(kn_dbconn *src, kn_dbconn *dst) {
    int bad = 0;
    out("%-26s %10s %10s  %s\n", "table", "sqlite", "postgres", "sha256 (normalised rows)");
    for (int i = 0; i < g_n; i++) {
        Table *t = &g_t[i];
        char *qs = src_select(t), *qd = dst_select(t);
        kn_db_result *rs = kn_dbconn_exec(src, qs, 0, NULL);
        kn_db_result *rd = kn_dbconn_exec(dst, qd, 0, NULL);
        free(qs);
        free(qd);
        if (!kn_db_ok(rs) || !kn_db_ok(rd)) {
            fprintf(stderr, "kn_dbcopy: verify %s: %s%s\n", t->name, kn_db_error(rs), kn_db_error(rd));
            kn_db_result_free(rs);
            kn_db_result_free(rd);
            return -1;
        }
        checksum(rs, t, t->src_sum, &t->src_rows);
        checksum(rd, t, t->dst_sum, &t->dst_rows);
        kn_db_result_free(rs);
        kn_db_result_free(rd);
        int same = t->src_rows == t->dst_rows && !strcmp(t->src_sum, t->dst_sum);
        bad += !same;
        out("%-26s %10lld %10lld  %.16s %s\n", t->name, (long long)t->src_rows, (long long)t->dst_rows, t->dst_sum,
            same ? "ok" : "MISMATCH");
    }
    return bad;
}

/* ------------------------------------------------------------------ copy */

static Table *find(const char *name) {
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_t[i].name, name)) return &g_t[i];
    return NULL;
}

static int load_tables(kn_dbconn *src, kn_dbconn *dst) {
    kn_db_result *r = kn_dbconn_exec(src,
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
        "AND name NOT IN ('kn_migrations', 'kn_pubsub') ORDER BY name", 0, NULL);
    if (!kn_db_ok(r)) return fail(r, "list sqlite tables");
    if (kn_db_rows(r) > MAX_TABLES) {
        fprintf(stderr, "kn_dbcopy: %d tables, more than MAX_TABLES (%d): refusing a partial copy\n", kn_db_rows(r), MAX_TABLES);
        kn_db_result_free(r);
        return -1;
    }
    for (int i = 0; i < kn_db_rows(r) && g_n < MAX_TABLES; i++) {
        Table *t = &g_t[g_n];
        memset(t, 0, sizeof *t);
        snprintf(t->name, sizeof t->name, "%s", kn_db_get(r, i, 0));
        if (!is_ident(t->name)) continue;
        g_n++;
    }
    kn_db_result_free(r);
    for (int i = 0; i < g_n; i++) {
        Table *t = &g_t[i];
        const char *name = t->name;
        kn_db_result *dc = kn_dbconn_exec(dst,
            "SELECT column_name, data_type, is_identity FROM information_schema.columns "
            "WHERE table_schema = current_schema() AND table_name = $1 AND is_generated = 'NEVER' "
            "ORDER BY ordinal_position", 1, &name);
        if (!kn_db_ok(dc)) return fail(dc, "postgres columns");
        if (kn_db_rows(dc) == 0) {
            fprintf(stderr, "kn_dbcopy: table %s is not in the PostgreSQL schema (migrate it first)\n", name);
            kn_db_result_free(dc);
            return -1;
        }
        char q[160];
        snprintf(q, sizeof q, "SELECT name, upper(type) FROM pragma_table_info('%s')", name);
        kn_db_result *sc = kn_dbconn_exec(src, q, 0, NULL);
        if (!kn_db_ok(sc)) {
            kn_db_result_free(dc);
            return fail(sc, "sqlite columns");
        }
        for (int c = 0; c < kn_db_rows(sc); c++) {
            const char *cn = kn_db_get(sc, c, 0), *st = kn_db_get(sc, c, 1);
            int found = -1;
            for (int k = 0; k < kn_db_rows(dc); k++)
                if (!strcmp(kn_db_get(dc, k, 0), cn)) found = k;
            if (found < 0) {
                fprintf(stderr, "kn_dbcopy: column %s.%s has no PostgreSQL counterpart\n", name, cn);
                kn_db_result_free(sc);
                kn_db_result_free(dc);
                return -1;
            }
            if (t->ncols >= MAX_COLS || !is_ident(cn)) {
                fprintf(stderr, "kn_dbcopy: unsupported column %s.%s\n", name, cn);
                kn_db_result_free(sc);
                kn_db_result_free(dc);
                return -1;
            }
            Col *col = &t->cols[t->ncols++];
            snprintf(col->name, sizeof col->name, "%s", cn);
            snprintf(col->pgtype, sizeof col->pgtype, "%s", kn_db_get(dc, found, 1));
            col->t = map_type(col->pgtype);
            col->src_real = strstr(st, "REAL") || strstr(st, "FLOA") || strstr(st, "DOUB");
            if (!strcmp(kn_db_get(dc, found, 2), "YES")) t->identity = 1;
        }
        for (int k = 0; k < kn_db_rows(dc); k++) { /* postgres-only columns keep their defaults */
            int have = 0;
            for (int c = 0; c < t->ncols; c++) have |= !strcmp(t->cols[c].name, kn_db_get(dc, k, 0));
            if (!have) out("note: %s.%s exists only in PostgreSQL (default kept)\n", name, kn_db_get(dc, k, 0));
        }
        kn_db_result_free(sc);
        kn_db_result_free(dc);
    }
    return 0;
}

/* Parents first: Kahn over the foreign keys between copied tables. */
static int order_tables(kn_dbconn *dst, Table **order) {
    kn_db_result *fk = kn_dbconn_exec(dst,
        "SELECT a.relname, b.relname FROM pg_constraint c JOIN pg_class a ON a.oid = c.conrelid "
        "JOIN pg_class b ON b.oid = c.confrelid WHERE c.contype = 'f' "
        "AND a.relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = current_schema())", 0, NULL);
    if (!kn_db_ok(fk)) return fail(fk, "foreign keys");
    int n = 0;
    while (n < g_n) {
        int progressed = 0;
        for (int i = 0; i < g_n; i++) {
            Table *t = &g_t[i];
            if (t->done) continue;
            int ready = 1;
            for (int k = 0; k < kn_db_rows(fk) && ready; k++) {
                if (strcmp(kn_db_get(fk, k, 0), t->name) || !strcmp(kn_db_get(fk, k, 1), t->name)) continue;
                Table *p = find(kn_db_get(fk, k, 1));
                if (p && !p->done) ready = 0;
            }
            if (ready) {
                t->done = 1;
                order[n++] = t;
                progressed = 1;
            }
        }
        if (!progressed) {
            kn_db_result_free(fk);
            fprintf(stderr, "kn_dbcopy: foreign key cycle between the copied tables\n");
            return -1;
        }
    }
    kn_db_result_free(fk);
    return 0;
}

/* Every SQLite value must fit its PostgreSQL column: integer / smallint columns are checked
 * against the source's min and max first, so a destination migrated with an older migration set
 * (players.player_premium before 0110_premium_bigint) is refused with the column named instead of
 * failing halfway through the copy. */
static int check_ranges(kn_dbconn *src) {
    int bad = 0;
    for (int i = 0; i < g_n; i++) {
        const Table *t = &g_t[i];
        for (int c = 0; c < t->ncols; c++) {
            const Col *col = &t->cols[c];
            long long lo, hi;
            if (!strcmp(col->pgtype, "integer")) lo = -2147483648LL, hi = 2147483647LL;
            else if (!strcmp(col->pgtype, "smallint")) lo = -32768, hi = 32767;
            else continue;
            char q[512];
            snprintf(q, sizeof q, "SELECT max(\"%s\"), min(\"%s\") FROM \"%s\" WHERE typeof(\"%s\") = 'integer'",
                     col->name, col->name, t->name, col->name);
            kn_db_result *r = kn_dbconn_exec(src, q, 0, NULL);
            if (!kn_db_ok(r)) return fail(r, "range check");
            if (kn_db_rows(r) > 0 && !kn_db_isnull(r, 0, 0)) {
                long long mx = kn_db_get_int64(r, 0, 0, 0), mn = kn_db_get_int64(r, 0, 1, 0);
                if (mx > hi || mn < lo) {
                    fprintf(stderr, "kn_dbcopy: %s.%s holds %lld, out of range for PostgreSQL %s: migrate the "
                            "destination with this release's migrations first\n", t->name, col->name,
                            mx > hi ? mx : mn, col->pgtype);
                    bad++;
                }
            }
            kn_db_result_free(r);
        }
    }
    return bad ? -1 : 0;
}

static void ts_param(const char *v, char *out, size_t cap) {
    /* SQLite stores UTC text without a zone: say so, else the server's TimeZone would apply */
    int has_zone = strchr(v + (strlen(v) > 10 ? 10 : 0), '+') || strchr(v, 'Z') ||
                   (strlen(v) > 19 && strchr(v + 19, '-'));
    snprintf(out, cap, has_zone ? "%s" : "%s+00", v);
}

static int copy_table(kn_dbconn *src, kn_dbconn *dst, Table *t) {
    char *qs = src_select(t);
    kn_db_result *rs = kn_dbconn_exec(src, qs, 0, NULL);
    free(qs);
    if (!kn_db_ok(rs)) return fail(rs, t->name);
    const int rows = kn_db_rows(rs), nc = t->ncols;
    int per = 60000 / (nc ? nc : 1);
    if (per > 500) per = 500;
    size_t cap = 4096 + (size_t)per * (size_t)nc * 48;
    char *sql = malloc(cap);
    const char **params = calloc((size_t)per * (size_t)nc, sizeof *params);
    char (*tsbuf)[48] = calloc((size_t)per * (size_t)nc, sizeof *tsbuf);
    for (int base = 0; base < rows; base += per) {
        int m = rows - base < per ? rows - base : per;
        size_t o = (size_t)snprintf(sql, cap, "INSERT INTO \"%s\" (", t->name);
        for (int c = 0; c < nc; c++) o += (size_t)snprintf(sql + o, cap - o, "%s\"%s\"", c ? ", " : "", t->cols[c].name);
        o += (size_t)snprintf(sql + o, cap - o, ")%s VALUES ", t->identity ? " OVERRIDING SYSTEM VALUE" : "");
        int p = 0;
        for (int i = 0; i < m; i++) {
            o += (size_t)snprintf(sql + o, cap - o, "%s(", i ? ", " : "");
            for (int c = 0; c < nc; c++) {
                const Col *col = &t->cols[c];
                const char *v = kn_db_isnull(rs, base + i, c) ? NULL : kn_db_get(rs, base + i, c);
                if (v && col->t == T_TS) {
                    ts_param(v, tsbuf[p], sizeof tsbuf[p]);
                    v = tsbuf[p];
                }
                if (v && col->t == T_BOOL) v = (!strcmp(v, "1") || !strcasecmp(v, "t") || !strcasecmp(v, "true")) ? "t" : "f";
                params[p] = v;
                p++;
                if (col->t == T_BYTEA) o += (size_t)snprintf(sql + o, cap - o, "%sdecode($%d, 'hex')", c ? ", " : "", p);
                else o += (size_t)snprintf(sql + o, cap - o, "%s$%d::%s", c ? ", " : "", p, col->pgtype);
            }
            o += (size_t)snprintf(sql + o, cap - o, ")");
        }
        kn_db_result *ri = kn_dbconn_exec(dst, sql, p, params);
        if (!kn_db_ok(ri)) {
            fprintf(stderr, "kn_dbcopy: insert into %s (rows %d..%d): %s\n", t->name, base, base + m - 1, kn_db_error(ri));
            kn_db_result_free(ri);
            kn_db_result_free(rs);
            free(sql);
            free(params);
            free(tsbuf);
            return -1;
        }
        kn_db_result_free(ri);
    }
    free(sql);
    free(params);
    free(tsbuf);
    kn_db_result_free(rs);
    return rows;
}

/* Identity / serial sequences past the copied ids (and SQLite's AUTOINCREMENT counter). */
static int fix_sequences(kn_dbconn *src, kn_dbconn *dst, Table *t) {
    for (int c = 0; c < t->ncols; c++) {
        if (t->cols[c].t != T_INT) continue;
        const char *args[2] = {t->name, t->cols[c].name};
        kn_db_result *r = kn_dbconn_exec(dst, "SELECT pg_get_serial_sequence(quote_ident($1), $2)", 2, args);
        if (!kn_db_ok(r)) return fail(r, "pg_get_serial_sequence");
        if (kn_db_rows(r) < 1 || kn_db_isnull(r, 0, 0)) {
            kn_db_result_free(r);
            continue;
        }
        char seq[256];
        snprintf(seq, sizeof seq, "%s", kn_db_get(r, 0, 0));
        kn_db_result_free(r);
        int64_t floor_v = 0;
        kn_db_result *ss = kn_dbconn_exec(src, "SELECT seq FROM sqlite_sequence WHERE name = $1", 1, args);
        if (kn_db_ok(ss) && kn_db_rows(ss) > 0) floor_v = kn_db_get_int64(ss, 0, 0, 0);
        kn_db_result_free(ss);
        char q[512], fl[32];
        snprintf(fl, sizeof fl, "%lld", (long long)floor_v);
        snprintf(q, sizeof q,
                 "SELECT setval($1, GREATEST(COALESCE((SELECT max(\"%s\") FROM \"%s\"), 0), $2::bigint, 1), "
                 "GREATEST(COALESCE((SELECT max(\"%s\") FROM \"%s\"), 0), $2::bigint) > 0)",
                 t->cols[c].name, t->name, t->cols[c].name, t->name);
        const char *sv[2] = {seq, fl};
        r = kn_dbconn_exec(dst, q, 2, sv);
        if (!kn_db_ok(r)) return fail(r, "setval");
        kn_db_result_free(r);
    }
    return 0;
}

static int migrations_ok(kn_dbconn *src, kn_dbconn *dst) {
    kn_db_result *s = kn_dbconn_exec(src, "SELECT version FROM kn_migrations ORDER BY version", 0, NULL);
    kn_db_result *d = kn_dbconn_exec(dst, "SELECT version::text FROM kn_migrations", 0, NULL);
    int ok = kn_db_ok(s) && kn_db_ok(d);
    if (!ok) fprintf(stderr, "kn_dbcopy: kn_migrations unreadable (migrate the PostgreSQL database first): %s%s\n",
                     kn_db_error(s), kn_db_error(d));
    for (int i = 0; ok && i < kn_db_rows(s); i++) {
        int have = 0;
        for (int k = 0; k < kn_db_rows(d); k++)
            have |= strtoll(kn_db_get(d, k, 0), NULL, 10) == strtoll(kn_db_get(s, i, 0), NULL, 10);
        if (!have) {
            fprintf(stderr, "kn_dbcopy: PostgreSQL lacks migration %s that the SQLite file has: migrate it first\n",
                    kn_db_get(s, i, 0));
            ok = 0;
        }
    }
    kn_db_result_free(s);
    kn_db_result_free(d);
    return ok;
}

static void usage(void) {
    printf("Usage: kn_dbcopy --sqlite FILE [--config F] [--set database.x=y ...] [--replace] [--verify-only] [--report FILE]\n"
           "Copies a coordinator SQLite database into the configured (migrated) PostgreSQL database in one\n"
           "transaction and verifies row counts + normalised row checksums. Exit 0 ok, 3 mismatch, 1 error.\n");
}

int main(int argc, char **argv) {
    kn_log_init("dbcopy");
    char err[512] = "";
    kn_config *cfg = kn_config_load(NULL, argc, argv, err, sizeof err);
    if (!cfg) {
        fprintf(stderr, "kn_dbcopy: config: %s\n", err);
        return 2;
    }
    const char *sqlite_path = NULL, *report = NULL;
    int replace = 0, verify_only = 0;
    for (int i = 1; i < argc; i++) {
        if (kn_config_arg_consumed(cfg, i)) continue;
        if (!strcmp(argv[i], "--sqlite") && i + 1 < argc) sqlite_path = argv[++i];
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else if (!strcmp(argv[i], "--replace")) replace = 1;
        else if (!strcmp(argv[i], "--verify-only")) verify_only = 1;
        else {
            usage();
            kn_config_free(cfg);
            return 2;
        }
    }
    struct stat st;
    if (!sqlite_path || stat(sqlite_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "kn_dbcopy: --sqlite FILE (an existing database file) is required\n");
        usage();
        kn_config_free(cfg);
        return 2;
    }
    if (report) g_report = fopen(report, "w");
    kn_db_config dc, sc;
    kn_db_config_from(cfg, &dc);
    if (!dc.driver || strcmp(dc.driver, "postgres") != 0) {
        fprintf(stderr, "kn_dbcopy: the destination (database.driver) must be postgres, not %s\n", dc.driver ? dc.driver : "?");
        kn_config_free(cfg);
        return 2;
    }
    memset(&sc, 0, sizeof sc);
    sc.driver = "sqlite";
    char uri[4200];
    snprintf(uri, sizeof uri, "file:%s?mode=ro", sqlite_path); /* read-only: never touch the source */
    sc.path = uri;
    sc.busy_timeout_ms = 5000;
    uint64_t t0 = kn_now_ms();
    kn_dbconn *src = kn_dbconn_open(&sc, err, sizeof err);
    if (!src) { /* drivers without URI filenames: plain path (still only read) */
        sc.path = sqlite_path;
        src = kn_dbconn_open(&sc, err, sizeof err);
    }
    if (!src) {
        fprintf(stderr, "kn_dbcopy: open %s: %s\n", sqlite_path, err);
        kn_config_free(cfg);
        return 1;
    }
    kn_dbconn *dst = kn_dbconn_open(&dc, err, sizeof err);
    if (!dst) {
        fprintf(stderr, "kn_dbcopy: connect to postgres: %s\n", err);
        kn_dbconn_close(src);
        kn_config_free(cfg);
        return 1;
    }
    int rc = 1;
    Table *order[MAX_TABLES];
    if (!migrations_ok(src, dst) || load_tables(src, dst) || order_tables(dst, order)) goto done;
    out("kn_dbcopy: %s -> postgres %s/%s (%d tables)\n", sqlite_path, dc.name ? dc.name : "",
        dc.search_path && *dc.search_path ? dc.search_path : "public", g_n);
    if (!verify_only) {
        kn_db_result *r = kn_dbconn_exec(dst, "SELECT count(*) FROM accounts", 0, NULL);
        int64_t have = kn_db_ok(r) && kn_db_rows(r) ? kn_db_get_int64(r, 0, 0, 0) : 0;
        kn_db_result_free(r);
        if (check_ranges(src)) goto done;
        if (have > 0 && !replace) {
            fprintf(stderr, "kn_dbcopy: the PostgreSQL database already has %lld accounts: refusing (--replace overwrites)\n",
                    (long long)have);
            goto done;
        }
        if (exec_ok(dst, "BEGIN", "begin") || exec_ok(dst, "SET LOCAL statement_timeout = 0", "timeout")) goto done;
        char *trunc = malloc((size_t)g_n * 72 + 64);
        size_t o = (size_t)sprintf(trunc, "TRUNCATE ");
        for (int i = 0; i < g_n; i++) o += (size_t)sprintf(trunc + o, "%s\"%s\"", i ? ", " : "", g_t[i].name);
        sprintf(trunc + o, " RESTART IDENTITY");
        int bad = exec_ok(dst, trunc, "truncate");
        free(trunc);
        for (int i = 0; !bad && i < g_n; i++) {
            char q[160];
            snprintf(q, sizeof q, "ALTER TABLE \"%.63s\" DISABLE TRIGGER USER", g_t[i].name);
            bad = exec_ok(dst, q, "disable triggers");
        }
        int64_t total = 0;
        for (int i = 0; !bad && i < g_n; i++) {
            int n = copy_table(src, dst, order[i]);
            if (n < 0) bad = 1;
            else total += n;
            if (!bad) bad = fix_sequences(src, dst, order[i]) != 0;
        }
        for (int i = 0; !bad && i < g_n; i++) {
            char q[160];
            snprintf(q, sizeof q, "ALTER TABLE \"%.63s\" ENABLE TRIGGER USER", g_t[i].name);
            bad = exec_ok(dst, q, "enable triggers");
        }
        if (bad) {
            exec_ok(dst, "ROLLBACK", "rollback");
            fprintf(stderr, "kn_dbcopy: copy failed: rolled back, nothing changed\n");
            goto done;
        }
        if (exec_ok(dst, "COMMIT", "commit")) goto done;
        out("copied %lld rows in %llu ms\n", (long long)total, (unsigned long long)(kn_now_ms() - t0));
    }
    int mismatches = verify(src, dst);
    if (mismatches < 0) goto done;
    out("verify: %s (%d table(s) differ), %llu ms total\n", mismatches ? "MISMATCH" : "identical", mismatches,
        (unsigned long long)(kn_now_ms() - t0));
    rc = mismatches ? 3 : 0;
done:
    kn_dbconn_close(dst);
    kn_dbconn_close(src);
    if (g_report) fclose(g_report);
    kn_config_free(cfg);
    return rc;
}
