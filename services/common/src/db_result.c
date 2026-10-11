/* kn_db_result accessors and JSON conversion (knsvc/db.h). */
#include "db_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PostgreSQL type OIDs (catalog/pg_type.h) */
#define OID_BOOL 16
#define OID_INT8 20
#define OID_INT2 21
#define OID_INT4 23
#define OID_OID 26
#define OID_JSON 114
#define OID_FLOAT4 700
#define OID_FLOAT8 701
#define OID_NUMERIC 1700
#define OID_JSONB 3802

kn_db_result *kn_db__result_error(const char *sqlstate, const char *msg) {
    kn_db_result *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    snprintf(r->sqlstate, sizeof r->sqlstate, "%s", sqlstate ? sqlstate : "");
    snprintf(r->error, sizeof r->error, "%s", msg ? msg : "error");
    return r;
}

kn_db_result *kn_db__result_from_pg(PGresult *pg, PGconn *conn) {
    kn_db_result *r = calloc(1, sizeof *r);
    if (!r) {
        PQclear(pg);
        return NULL;
    }
    r->pg = pg;
    ExecStatusType st = pg ? PQresultStatus(pg) : PGRES_FATAL_ERROR;
    r->ok = st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK || st == PGRES_EMPTY_QUERY;
    if (!r->ok) {
        const char *state = pg ? PQresultErrorField(pg, PG_DIAG_SQLSTATE) : NULL;
        const char *msg = pg ? PQresultErrorMessage(pg) : (conn ? PQerrorMessage(conn) : "no result");
        snprintf(r->sqlstate, sizeof r->sqlstate, "%s", state ? state : (pg ? "" : "08006"));
        snprintf(r->error, sizeof r->error, "%s", msg && *msg ? msg : "query failed");
        size_t n = strlen(r->error);
        while (n && (r->error[n - 1] == '\n' || r->error[n - 1] == ' ')) r->error[--n] = 0;
    }
    return r;
}

void kn_db_result_free(kn_db_result *r) {
    if (!r) return;
    PQclear(r->pg);
    free(r);
}

int kn_db_ok(const kn_db_result *r) { return r && r->ok; }
const char *kn_db_error(const kn_db_result *r) { return !r ? "no result" : r->ok ? "" : r->error; }
const char *kn_db_sqlstate(const kn_db_result *r) { return r && !r->ok ? r->sqlstate : ""; }
int kn_db_is_unique_violation(const kn_db_result *r) { return r && !r->ok && strcmp(r->sqlstate, "23505") == 0; }
int kn_db_rows(const kn_db_result *r) { return r && r->pg && r->ok ? PQntuples(r->pg) : 0; }
int kn_db_cols(const kn_db_result *r) { return r && r->pg && r->ok ? PQnfields(r->pg) : 0; }
void *kn_db_result_pg(const kn_db_result *r) { return r ? r->pg : NULL; }

int64_t kn_db_affected(const kn_db_result *r) {
    if (!r || !r->pg || !r->ok) return 0;
    const char *s = PQcmdTuples(r->pg);
    return s && *s ? strtoll(s, NULL, 10) : 0;
}

int kn_db_col(const kn_db_result *r, const char *name) {
    return r && r->pg && r->ok ? PQfnumber(r->pg, name) : -1;
}

const char *kn_db_colname(const kn_db_result *r, int col) {
    return col >= 0 && col < kn_db_cols(r) ? PQfname(r->pg, col) : NULL;
}

static int in_range(const kn_db_result *r, int row, int col) {
    return row >= 0 && row < kn_db_rows(r) && col >= 0 && col < kn_db_cols(r);
}

int kn_db_isnull(const kn_db_result *r, int row, int col) {
    return !in_range(r, row, col) || PQgetisnull(r->pg, row, col);
}

const char *kn_db_get(const kn_db_result *r, int row, int col) {
    return in_range(r, row, col) ? PQgetvalue(r->pg, row, col) : "";
}

const char *kn_db_get_by(const kn_db_result *r, int row, const char *col) {
    int c = kn_db_col(r, col);
    return c < 0 ? NULL : kn_db_get(r, row, c);
}

int64_t kn_db_get_int64(const kn_db_result *r, int row, int col, int64_t def) {
    if (kn_db_isnull(r, row, col)) return def;
    char *end;
    const char *s = PQgetvalue(r->pg, row, col);
    long long v = strtoll(s, &end, 10);
    return end == s ? def : v;
}

double kn_db_get_double(const kn_db_result *r, int row, int col, double def) {
    if (kn_db_isnull(r, row, col)) return def;
    char *end;
    const char *s = PQgetvalue(r->pg, row, col);
    double v = strtod(s, &end);
    return end == s ? def : v;
}

int kn_db_get_bool(const kn_db_result *r, int row, int col, int def) {
    if (kn_db_isnull(r, row, col)) return def;
    const char *s = PQgetvalue(r->pg, row, col);
    if (s[0] == 't' || s[0] == '1') return 1;
    if (s[0] == 'f' || s[0] == '0') return 0;
    return def;
}

static cJSON *cell_json(const kn_db_result *r, int row, int col) {
    if (PQgetisnull(r->pg, row, col)) return cJSON_CreateNull();
    const char *s = PQgetvalue(r->pg, row, col);
    switch (PQftype(r->pg, col)) {
    case OID_BOOL: return cJSON_CreateBool(s[0] == 't');
    case OID_INT2: case OID_INT4: case OID_OID: return cJSON_CreateNumber(strtod(s, NULL));
    case OID_INT8: {
        long long v = strtoll(s, NULL, 10);
        /* beyond 2^53 a JSON number loses precision: keep the text */
        if (v > 9007199254740992LL || v < -9007199254740992LL) return cJSON_CreateString(s);
        return cJSON_CreateNumber((double)v);
    }
    case OID_FLOAT4: case OID_FLOAT8: case OID_NUMERIC: {
        double d = strtod(s, NULL);
        return isfinite(d) ? cJSON_CreateNumber(d) : cJSON_CreateString(s);
    }
    case OID_JSON: case OID_JSONB: {
        cJSON *j = cJSON_Parse(s);
        return j ? j : cJSON_CreateString(s);
    }
    default: return cJSON_CreateString(s);
    }
}

cJSON *kn_db_row_to_json(const kn_db_result *r, int row) {
    if (row < 0 || row >= kn_db_rows(r)) return NULL;
    cJSON *o = cJSON_CreateObject();
    for (int c = 0; o && c < kn_db_cols(r); c++) cJSON_AddItemToObject(o, PQfname(r->pg, c), cell_json(r, row, c));
    return o;
}

cJSON *kn_db_result_to_json(const kn_db_result *r) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < kn_db_rows(r); i++) cJSON_AddItemToArray(a, kn_db_row_to_json(r, i));
    return a;
}
