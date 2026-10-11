/* SQL text helpers of the store layer (knsvc/store.h): kn_sql variant
 * selection, {macro} expansion, placeholder rewriting, parameter typing. */
#include "store_internal.h"
#include "knsvc/buf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ macros */

static const kn_sql_macro *find_macro(const kn_sql_macro *m, const char *name, size_t len) {
    for (; m && m->name; m++)
        if (strlen(m->name) == len && !strncmp(m->name, name, len)) return m;
    return NULL;
}

/* Length of a quoted literal starting at s[0] (' or "), doubled quotes escaped. */
static size_t quoted_len(const char *s) {
    char q = s[0];
    size_t i = 1;
    for (;;) {
        if (!s[i]) return i;
        if (s[i] == q) {
            if (s[i + 1] == q) {
                i += 2;
                continue;
            }
            return i + 1;
        }
        i++;
    }
}

#define MAX_ARGS 9

/* Parse "{name:args}" args starting after ':' at s; returns index of the closing
 * '}' (or 0 when unterminated); fills arg [start,len) ranges. */
static size_t parse_args(const char *s, size_t from, size_t *starts, size_t *lens, int *nargs) {
    int depth = 0;
    size_t i = from, a0 = from;
    *nargs = 0;
    while (s[i]) {
        char c = s[i];
        if (c == '\'' || c == '"') {
            i += quoted_len(s + i);
            continue;
        }
        if (c == '(' || c == '{' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth > 0) depth--;
        else if (c == '}') {
            if (depth == 0) {
                if (*nargs < MAX_ARGS) {
                    starts[*nargs] = a0;
                    lens[*nargs] = i - a0;
                    (*nargs)++;
                }
                return i;
            }
            depth--;
        } else if (c == ',' && depth == 0 && *nargs < MAX_ARGS - 1) {
            starts[*nargs] = a0;
            lens[*nargs] = i - a0;
            (*nargs)++;
            a0 = i + 1;
        }
        i++;
    }
    return 0;
}

static char *trim_dup(const char *s, size_t len) {
    while (len && isspace((unsigned char)*s)) s++, len--;
    while (len && isspace((unsigned char)s[len - 1])) len--;
    char *d = malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = 0;
    return d;
}

char *kn_sql_expand(const char *text, const kn_sql_macro *macros) {
    if (!text) return NULL;
    kn_buf out = {0};
    size_t i = 0;
    while (text[i]) {
        char c = text[i];
        if (c == '\'' || c == '"') {
            size_t n = quoted_len(text + i);
            kn_buf_append(&out, text + i, n);
            i += n;
            continue;
        }
        if (c == '{' && (islower((unsigned char)text[i + 1]) || text[i + 1] == '_')) {
            size_t n = 1;
            while (isalnum((unsigned char)text[i + n]) || text[i + n] == '_') n++;
            const kn_sql_macro *m = (text[i + n] == '}' || text[i + n] == ':') ? find_macro(macros, text + i + 1, n - 1) : NULL;
            if (m) {
                char *args[MAX_ARGS] = {0};
                int nargs = 0;
                size_t end = i + n;
                if (text[i + n] == ':') {
                    size_t st[MAX_ARGS], ln[MAX_ARGS];
                    end = parse_args(text, i + n + 1, st, ln, &nargs);
                    if (!end) { /* unterminated: copy as is */
                        kn_buf_append_byte(&out, '{');
                        i++;
                        continue;
                    }
                    for (int k = 0; k < nargs; k++) {
                        char *raw = trim_dup(text + st[k], ln[k]);
                        args[k] = raw ? kn_sql_expand(raw, macros) : NULL;
                        free(raw);
                    }
                }
                for (const char *e = m->expansion; *e; e++) {
                    if (e[0] == '%' && e[1] >= '1' && e[1] <= '9') {
                        int k = e[1] - '1';
                        if (k < nargs && args[k]) kn_buf_append_str(&out, args[k]);
                        e++;
                    } else if (e[0] == '%' && e[1] == '*') {
                        for (int k = 0; k < nargs; k++) {
                            if (k) kn_buf_append_str(&out, ", ");
                            if (args[k]) kn_buf_append_str(&out, args[k]);
                        }
                        e++;
                    } else {
                        kn_buf_append_byte(&out, (uint8_t)*e);
                    }
                }
                for (int k = 0; k < nargs; k++) free(args[k]);
                i = end + 1;
                continue;
            }
        }
        kn_buf_append_byte(&out, (uint8_t)c);
        i++;
    }
    if (!out.data) kn_buf_reserve(&out, 1);
    return kn_buf_detach(&out, NULL);
}

char *kn_sql_resolve(const kn_sql *sql, const kn_store_driver *drv) {
    if (!sql || !drv) return NULL;
    const char *d = drv->dialect ? drv->dialect : drv->name;
    const char *text = !strcmp(d, "postgres") ? sql->pg : !strcmp(d, "sqlite") ? sql->sqlite
                     : !strcmp(d, "mysql") ? sql->mysql : NULL;
    if (text && !strcmp(d, "postgres")) return strdup(text); /* today's PostgreSQL text, verbatim */
    if (!text) text = sql->any;
    return text ? kn_sql_expand(text, drv->macros) : NULL;
}

/* ------------------------------------------------------------------ placeholders */

typedef struct inline_ctx {
    int nparams;
    const char *const *params;
    kn_store_escape_fn escape;
    void *ud;
} inline_ctx;

/* Rewrites every $n outside quotes/comments; copies everything else.
 * mode 0: ?n   mode 1: ? (+ order / limit_ctx)   mode 2: inline literal (ic). */
static char *rewrite_params_ex(const char *sql, int mode, int **order, unsigned char **limit_ctx, int *count,
                               const inline_ctx *ic) {
    kn_buf out = {0};
    size_t len = strlen(sql);
    int *ord = NULL;
    unsigned char *lim = NULL;
    int n = 0;
    if (mode == 1) {
        ord = malloc((len + 1) * sizeof *ord);
        lim = malloc(len + 1);
        if (!ord || !lim) {
            free(ord);
            free(lim);
            return NULL;
        }
    }
    size_t i = 0;
    while (i < len) {
        char c = sql[i];
        if (c == '\'' || c == '"' || c == '`') {
            size_t q;
            if (c == '`') {
                q = 1;
                while (i + q < len && sql[i + q] != '`') q++;
                if (i + q < len) q++;
            } else {
                q = quoted_len(sql + i);
            }
            kn_buf_append(&out, sql + i, q);
            i += q;
            continue;
        }
        if (c == '-' && sql[i + 1] == '-') { /* comment to end of line */
            size_t q = 0;
            while (i + q < len && sql[i + q] != '\n') q++;
            kn_buf_append(&out, sql + i, q);
            i += q;
            continue;
        }
        if (c == '$' && isdigit((unsigned char)sql[i + 1]) && (i == 0 || !isalnum((unsigned char)sql[i - 1]))) {
            size_t q = 1;
            int idx = 0;
            while (isdigit((unsigned char)sql[i + q])) idx = idx * 10 + (sql[i + q++] - '0');
            if (mode == 0) {
                kn_buf_appendf(&out, "?%d", idx);
            } else if (mode == 2) {
                size_t p = out.len;
                while (p && isspace((unsigned char)out.data[p - 1])) p--;
                size_t w = p;
                while (w && isalpha((unsigned char)out.data[w - 1])) w--;
                int is_lim = (p - w == 5 && !strncasecmp(out.data + w, "LIMIT", 5)) ||
                             (p - w == 6 && !strncasecmp(out.data + w, "OFFSET", 6));
                const char *v = idx >= 1 && idx <= ic->nparams ? ic->params[idx - 1] : NULL;
                int64_t iv;
                if (!v) {
                    kn_buf_append_str(&out, "NULL");
                } else if (is_lim && kn_store_param_int(v, &iv)) {
                    kn_buf_appendf(&out, "%lld", (long long)iv);
                } else {
                    size_t vl = strlen(v);
                    if (kn_buf_reserve(&out, vl * 2 + 3) != 0) {
                        kn_buf_free(&out);
                        return NULL;
                    }
                    out.data[out.len++] = '\'';
                    out.len += ic->escape(ic->ud, out.data + out.len, v, vl);
                    out.data[out.len++] = '\'';
                    out.data[out.len] = 0;
                }
            } else {
                /* LIMIT/OFFSET context: previous word */
                size_t p = out.len;
                while (p && isspace((unsigned char)out.data[p - 1])) p--;
                size_t w = p;
                while (w && isalpha((unsigned char)out.data[w - 1])) w--;
                int is_lim = (p - w == 5 && !strncasecmp(out.data + w, "LIMIT", 5)) ||
                             (p - w == 6 && !strncasecmp(out.data + w, "OFFSET", 6));
                ord[n] = idx - 1;
                lim[n] = (unsigned char)is_lim;
                n++;
                kn_buf_append_byte(&out, '?');
            }
            i += q;
            continue;
        }
        kn_buf_append_byte(&out, (uint8_t)c);
        i++;
    }
    if (!out.data) kn_buf_reserve(&out, 1);
    if (mode == 1) {
        *order = ord;
        if (limit_ctx) *limit_ctx = lim;
        else free(lim);
        *count = n;
    }
    return kn_buf_detach(&out, NULL);
}

char *kn_store_sql_numbered(const char *sql) { return rewrite_params_ex(sql, 0, NULL, NULL, NULL, NULL); }

char *kn_store_sql_qmark(const char *sql, int **order, unsigned char **limit_ctx, int *count) {
    return rewrite_params_ex(sql, 1, order, limit_ctx, count, NULL);
}

char *kn_store_sql_inline(const char *sql, int nparams, const char *const *params, kn_store_escape_fn escape,
                          void *ud) {
    inline_ctx ic = {nparams, params, escape, ud};
    return rewrite_params_ex(sql, 2, NULL, NULL, NULL, &ic);
}

/* ------------------------------------------------------------------ values */

int kn_store_param_int(const char *s, int64_t *out) {
    if (!s || !*s) return 0;
    const char *p = s;
    if (*p == '-') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    if (*p == '0' && p[1]) return 0;
    if (*p == '0' && s[0] == '-') return 0; /* "-0" */
    size_t digits = 0;
    for (const char *q = p; *q; q++) {
        if (!isdigit((unsigned char)*q)) return 0;
        digits++;
    }
    if (digits > 18) return 0; /* keep clear of int64 overflow */
    if (out) *out = strtoll(s, NULL, 10);
    return 1;
}

int kn_store_alias_type(char *name) {
    size_t n = strlen(name);
    if (n < 4 || name[n - 3] != '_' || name[n - 2] != '_') return -1;
    int t;
    switch (name[n - 1]) {
    case 'j': t = KN_DB_T_JSON; break;
    case 'b': t = KN_DB_T_BOOL; break;
    case 'i': t = KN_DB_T_INT; break;
    case 'f': t = KN_DB_T_FLOAT; break;
    case 't': t = KN_DB_T_TEXT; break;
    default: return -1;
    }
    name[n - 3] = 0;
    return t;
}

int kn_store_parse_bool(const char *s, int def) {
    if (!s || !*s) return def;
    if (!strcasecmp(s, "t") || !strcasecmp(s, "true") || !strcmp(s, "1") || !strcasecmp(s, "y") ||
        !strcasecmp(s, "yes") || !strcasecmp(s, "on"))
        return 1;
    if (!strcasecmp(s, "f") || !strcasecmp(s, "false") || !strcmp(s, "0") || !strcasecmp(s, "n") ||
        !strcasecmp(s, "no") || !strcasecmp(s, "off"))
        return 0;
    /* pg compat: first letter t/f, digits */
    if (s[0] == 't' || s[0] == '1') return 1;
    if (s[0] == 'f' || s[0] == '0') return 0;
    return def;
}
