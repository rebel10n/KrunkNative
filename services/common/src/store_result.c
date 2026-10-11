/* kn_db_result for every driver (knsvc/db.h accessors, knsvc/store.h builders).
 * A result is either a driver's native object behind kn_store_result_ops
 * (postgres PGresult) or materialized here (rows of copied text cells). */
#include "store_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct kn_db_result {
    int ok;
    char sqlstate[6];
    char error[512];
    char *user_msg;
    int64_t affected;
    int64_t last_id;
    const kn_store_result_ops *ops;
    void *native;
    /* materialized */
    int ncols, nrows, rowcap;
    char **names;
    kn_db_type *types;
    char **cells;      /* nrows * ncols, NULL = SQL NULL */
};

/* ------------------------------------------------------------------ builders */

kn_db_result *kn_db_result_error(const char *sqlstate, const char *msg) {
    kn_db_result *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    snprintf(r->sqlstate, sizeof r->sqlstate, "%s", sqlstate ? sqlstate : "");
    snprintf(r->error, sizeof r->error, "%s", msg && *msg ? msg : "error");
    size_t n = strlen(r->error);
    while (n && (r->error[n - 1] == '\n' || r->error[n - 1] == ' ')) r->error[--n] = 0;
    return r;
}

kn_db_result *kn_db_result_user_error(const char *msg) {
    kn_db_result *r = kn_db_result_error("P0001", msg);
    if (r) kn_db_result_set_user_message(r, msg);
    return r;
}

kn_db_result *kn_db_result_command(int64_t affected) {
    kn_db_result *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->ok = 1;
    r->affected = affected;
    return r;
}

kn_db_result *kn_db_result_new(int ncols, const char *const *names, const kn_db_type *types) {
    kn_db_result *r = kn_db_result_command(0);
    if (!r) return NULL;
    if (ncols > 0) {
        r->names = calloc((size_t)ncols, sizeof *r->names);
        r->types = calloc((size_t)ncols, sizeof *r->types);
        if (!r->names || !r->types) {
            kn_db_result_free(r);
            return NULL;
        }
        r->ncols = ncols;
        for (int i = 0; i < ncols; i++) {
            r->names[i] = strdup(names && names[i] ? names[i] : "?column?");
            if (!r->names[i]) {
                kn_db_result_free(r);
                return NULL;
            }
            r->types[i] = types ? types[i] : KN_DB_T_TEXT;
        }
    }
    return r;
}

int kn_db_result_add_row_n(kn_db_result *r, const char *const *cells, const size_t *lens) {
    if (!r || r->ops) return -1;
    if (r->nrows == r->rowcap) {
        int cap = r->rowcap ? r->rowcap * 2 : 8;
        char **n = realloc(r->cells, (size_t)cap * (size_t)(r->ncols ? r->ncols : 1) * sizeof *n);
        if (!n) return -1;
        r->cells = n;
        r->rowcap = cap;
    }
    char **row = r->cells + (size_t)r->nrows * (size_t)r->ncols;
    for (int c = 0; c < r->ncols; c++) {
        row[c] = NULL;
        if (!cells[c]) continue;
        size_t len = lens ? lens[c] : strlen(cells[c]);
        row[c] = malloc(len + 1);
        if (!row[c]) {
            for (int k = 0; k < c; k++) free(row[k]);
            return -1;
        }
        memcpy(row[c], cells[c], len);
        row[c][len] = 0;
    }
    r->nrows++;
    return 0;
}

int kn_db_result_add_row(kn_db_result *r, const char *const *cells) { return kn_db_result_add_row_n(r, cells, NULL); }

kn_db_result *kn_db_result_text(const char *name, kn_db_type type, const char *value) {
    const char *names[1] = {name};
    kn_db_type types[1] = {type};
    kn_db_result *r = kn_db_result_new(1, names, types);
    const char *cells[1] = {value};
    if (r && kn_db_result_add_row(r, cells) != 0) {
        kn_db_result_free(r);
        return kn_db_result_error("53200", "out of memory");
    }
    return r ? r : kn_db_result_error("53200", "out of memory");
}

kn_db_result *kn_db_result_int(const char *name, int64_t v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    return kn_db_result_text(name, KN_DB_T_INT, buf);
}

kn_db_result *kn_db_result_json(const char *name, cJSON *v) {
    char *s = v ? cJSON_PrintUnformatted(v) : NULL;
    cJSON_Delete(v);
    if (v && !s) return kn_db_result_error("53200", "out of memory");
    kn_db_result *r = kn_db_result_text(name, KN_DB_T_JSON, s);
    free(s);
    return r;
}

void kn_db_result_set_type(kn_db_result *r, int col, kn_db_type type) {
    if (r && !r->ops && col >= 0 && col < r->ncols) r->types[col] = type;
}

kn_db_result *kn_db_result_native(const kn_store_result_ops *ops, void *native, int64_t affected) {
    kn_db_result *r = kn_db_result_command(affected);
    if (!r) {
        if (ops && ops->free && native) ops->free(native);
        return NULL;
    }
    r->ops = ops;
    r->native = native;
    return r;
}

void *kn_db_result_native_ptr(const kn_db_result *r, const kn_store_result_ops *ops) {
    return r && r->ops == ops ? r->native : NULL;
}

void kn_db__result_attach(kn_db_result *r, const kn_store_result_ops *ops, void *native) {
    if (!r) return;
    if (r->ops && r->ops->free && r->native) r->ops->free(r->native);
    r->ops = ops;
    r->native = native;
}

void kn_db_result_set_user_message(kn_db_result *r, const char *msg) {
    if (!r) return;
    free(r->user_msg);
    r->user_msg = msg ? strdup(msg) : NULL;
}

void kn_db_result_set_affected(kn_db_result *r, int64_t affected) {
    if (r) r->affected = affected;
}

void kn_db_result_set_last_id(kn_db_result *r, int64_t id) {
    if (r) r->last_id = id;
}

void kn_db_result_free(kn_db_result *r) {
    if (!r) return;
    if (r->ops && r->ops->free && r->native) r->ops->free(r->native);
    for (size_t i = 0; i < (size_t)r->nrows * (size_t)r->ncols; i++) free(r->cells[i]);
    free(r->cells);
    for (int i = 0; i < r->ncols; i++) free(r->names ? r->names[i] : NULL);
    free(r->names);
    free(r->types);
    free(r->user_msg);
    free(r);
}

/* ------------------------------------------------------------------ status */

int kn_db_ok(const kn_db_result *r) { return r && r->ok; }
const char *kn_db_error(const kn_db_result *r) { return !r ? "no result" : r->ok ? "" : r->error; }
const char *kn_db_sqlstate(const kn_db_result *r) { return r && !r->ok ? r->sqlstate : ""; }
int kn_db_is_unique_violation(const kn_db_result *r) { return r && !r->ok && strcmp(r->sqlstate, "23505") == 0; }
int64_t kn_db_last_id(const kn_db_result *r) { return r && r->ok ? r->last_id : 0; }

const char *kn_db_user_message(const kn_db_result *r) {
    if (!r || r->ok || strcmp(r->sqlstate, "P0001") != 0) return NULL;
    return r->user_msg && *r->user_msg ? r->user_msg : r->error;
}

kn_db_errclass kn_db_errclass_of(const kn_db_result *r) {
    if (!r) return KN_DB_E_OTHER;
    if (r->ok) return KN_DB_E_NONE;
    const char *s = r->sqlstate;
    if (!strcmp(s, "23505")) return KN_DB_E_UNIQUE;
    if (!strncmp(s, "23", 2)) return KN_DB_E_CONSTRAINT;
    if (!strcmp(s, "P0001")) return KN_DB_E_USER;
    if (!strncmp(s, "08", 2)) return KN_DB_E_CONNECTION;
    if (!strcmp(s, "40001") || !strcmp(s, "40P01") || !strcmp(s, "55P03")) return KN_DB_E_RETRY;
    if (!strcmp(s, "25P02") || !strcmp(s, "40000")) return KN_DB_E_TX_ABORTED;
    if (!strcmp(s, "57014")) return KN_DB_E_TIMEOUT;
    if (!strncmp(s, "42", 2)) return KN_DB_E_SYNTAX;
    return KN_DB_E_OTHER;
}

/* ------------------------------------------------------------------ accessors */

int kn_db_rows(const kn_db_result *r) {
    if (!r || !r->ok) return 0;
    return r->ops ? r->ops->rows(r->native) : r->nrows;
}

int kn_db_cols(const kn_db_result *r) {
    if (!r || !r->ok) return 0;
    return r->ops ? r->ops->cols(r->native) : r->ncols;
}

int64_t kn_db_affected(const kn_db_result *r) { return r && r->ok ? r->affected : 0; }

int kn_db_col(const kn_db_result *r, const char *name) {
    if (!r || !r->ok || !name) return -1;
    if (r->ops) return r->ops->colnum(r->native, name);
    for (int i = 0; i < r->ncols; i++)
        if (!strcmp(r->names[i], name)) return i;
    return -1;
}

const char *kn_db_colname(const kn_db_result *r, int col) {
    if (col < 0 || col >= kn_db_cols(r)) return NULL;
    return r->ops ? r->ops->colname(r->native, col) : r->names[col];
}

kn_db_type kn_db_coltype(const kn_db_result *r, int col) {
    if (col < 0 || col >= kn_db_cols(r)) return KN_DB_T_TEXT;
    return r->ops ? r->ops->coltype(r->native, col) : r->types[col];
}

static int in_range(const kn_db_result *r, int row, int col) {
    return row >= 0 && row < kn_db_rows(r) && col >= 0 && col < kn_db_cols(r);
}

int kn_db_isnull(const kn_db_result *r, int row, int col) {
    if (!in_range(r, row, col)) return 1;
    return r->ops ? r->ops->isnull(r->native, row, col) : r->cells[(size_t)row * (size_t)r->ncols + (size_t)col] == NULL;
}

const char *kn_db_get(const kn_db_result *r, int row, int col) {
    if (!in_range(r, row, col)) return "";
    if (r->ops) return r->ops->get(r->native, row, col);
    const char *s = r->cells[(size_t)row * (size_t)r->ncols + (size_t)col];
    return s ? s : "";
}

const char *kn_db_get_by(const kn_db_result *r, int row, const char *col) {
    int c = kn_db_col(r, col);
    return c < 0 ? NULL : kn_db_get(r, row, c);
}

int64_t kn_db_get_int64(const kn_db_result *r, int row, int col, int64_t def) {
    if (kn_db_isnull(r, row, col)) return def;
    char *end;
    const char *s = kn_db_get(r, row, col);
    long long v = strtoll(s, &end, 10);
    return end == s ? def : v;
}

double kn_db_get_double(const kn_db_result *r, int row, int col, double def) {
    if (kn_db_isnull(r, row, col)) return def;
    char *end;
    const char *s = kn_db_get(r, row, col);
    double v = strtod(s, &end);
    return end == s ? def : v;
}

int kn_db_get_bool(const kn_db_result *r, int row, int col, int def) {
    if (kn_db_isnull(r, row, col)) return def;
    return kn_store_parse_bool(kn_db_get(r, row, col), def);
}

/* ------------------------------------------------------------------ JSON */

static cJSON *cell_json(const kn_db_result *r, int row, int col) {
    if (kn_db_isnull(r, row, col)) return cJSON_CreateNull();
    const char *s = kn_db_get(r, row, col);
    switch (kn_db_coltype(r, col)) {
    case KN_DB_T_BOOL: return cJSON_CreateBool(kn_store_parse_bool(s, 0));
    case KN_DB_T_INT: {
        char *end;
        long long v = strtoll(s, &end, 10);
        if (end == s) return cJSON_CreateString(s);
        if (*end == '.') return cJSON_CreateNumber(strtod(s, NULL)); /* "2.0" from a REAL */
        /* beyond 2^53 a JSON number loses precision: keep the text */
        if (v > 9007199254740992LL || v < -9007199254740992LL) return cJSON_CreateString(s);
        return cJSON_CreateNumber((double)v);
    }
    case KN_DB_T_FLOAT: {
        double d = strtod(s, NULL);
        return isfinite(d) ? cJSON_CreateNumber(d) : cJSON_CreateString(s);
    }
    case KN_DB_T_JSON: {
        cJSON *j = cJSON_Parse(s);
        return j ? j : cJSON_CreateString(s);
    }
    default: return cJSON_CreateString(s);
    }
}

cJSON *kn_db_row_to_json(const kn_db_result *r, int row) {
    if (row < 0 || row >= kn_db_rows(r)) return NULL;
    cJSON *o = cJSON_CreateObject();
    for (int c = 0; o && c < kn_db_cols(r); c++) cJSON_AddItemToObject(o, kn_db_colname(r, c), cell_json(r, row, c));
    return o;
}

cJSON *kn_db_result_to_json(const kn_db_result *r) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < kn_db_rows(r); i++) cJSON_AddItemToArray(a, kn_db_row_to_json(r, i));
    return a;
}
