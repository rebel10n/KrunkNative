/*
 * services/brain/v10_pack.c - the packed JSON encoder (v10_pack.h). A port of v10.js "pack"
 * (27744-27912): pass 1 collects the dictionaries, pass 2 writes the tree with their final
 * offsets. Dictionary lookups use a hash table so a 8000-skin body stays linear.
 */
#include "v10_pack.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len, cap;
    int oom;
} Out;

typedef struct {
    char **s;          /* escaped strings, dictionary order */
    size_t n;
    size_t *slot;      /* hash table: index+1 into s (0 = empty) */
    size_t cap;       /* table size (power of two) */
    int oom;
} StrDict;

typedef struct {
    int64_t *v;
    size_t n, cap;
    int oom;
} IntDict;

typedef struct {
    double *v;
    size_t n, cap;
    int oom;
} FltDict;

typedef struct {
    StrDict strings;
    IntDict ints;
    FltDict floats;
} Dicts;

static void out_put(Out *o, const char *s, size_t n) {
    if (o->oom) return;
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (cap < o->len + n + 1) cap *= 2;
        char *nb = realloc(o->buf, cap);
        if (!nb) {
            o->oom = 1;
            return;
        }
        o->buf = nb;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}

static void out_str(Out *o, const char *s) { out_put(o, s, strlen(s)); }

static void out_char(Out *o, char c) { out_put(o, &c, 1); }

/* JS Number.prototype.toString(36).toUpperCase() */
static void out_base36(Out *o, int64_t v) {
    char tmp[32];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t) (-(v + 1)) + 1 : (uint64_t) v;
    do {
        int d = (int) (u % 36);
        tmp[n++] = (char) (d < 10 ? '0' + d : 'A' + d - 10);
        u /= 36;
    } while (u);
    if (v < 0) out_char(o, '-');
    while (n) out_char(o, tmp[--n]);
}

/* the escape of v10.js _10544: " " -> "+", "+" -> "%2B", "|" -> "%7C", "^" -> "%5E", "%" -> "%25" */
static char *escape_dup(const char *s) {
    size_t n = 0;
    for (const char *p = s; *p; p++) n += (*p == '+' || *p == '|' || *p == '^' || *p == '%') ? 3 : 1;
    char *o = malloc(n + 1);
    if (!o) return NULL;
    char *w = o;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case ' ': *w++ = '+'; break;
            case '+': memcpy(w, "%2B", 3); w += 3; break;
            case '|': memcpy(w, "%7C", 3); w += 3; break;
            case '^': memcpy(w, "%5E", 3); w += 3; break;
            case '%': memcpy(w, "%25", 3); w += 3; break;
            default: *w++ = *p;
        }
    }
    *w = 0;
    return o;
}

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char) *s;
        h *= 1099511628211ULL;
    }
    return h;
}

static void strdict_grow(StrDict *d) {
    size_t cap = d->cap ? d->cap * 2 : 1024;
    size_t *slot = calloc(cap, sizeof *slot);
    if (!slot) {
        d->oom = 1;
        return;
    }
    for (size_t i = 0; i < d->n; i++) {
        size_t h = fnv1a(d->s[i]) & (cap - 1);
        while (slot[h]) h = (h + 1) & (cap - 1);
        slot[h] = i + 1;
    }
    free(d->slot);
    d->slot = slot;
    d->cap = cap;
}

/* index of the escaped string esc in the dictionary, adding it when new; -1 on out of memory */
static long strdict_index(StrDict *d, const char *esc) {
    if (d->oom) return -1;
    if (!d->cap || (d->n + 1) * 2 > d->cap) strdict_grow(d);
    if (d->oom) return -1;
    size_t h = fnv1a(esc) & (d->cap - 1);
    while (d->slot[h]) {
        if (!strcmp(d->s[d->slot[h] - 1], esc)) return (long) d->slot[h] - 1;
        h = (h + 1) & (d->cap - 1);
    }
    if (d->n == 0 || d->n % 256 == 0) {
        char **ns = realloc(d->s, (d->n + 256) * sizeof *ns);
        if (!ns) {
            d->oom = 1;
            return -1;
        }
        d->s = ns;
    }
    const size_t el = strlen(esc);
    char *copy = malloc(el + 1);
    if (copy) memcpy(copy, esc, el + 1);
    if (!copy) {
        d->oom = 1;
        return -1;
    }
    d->s[d->n] = copy;
    d->slot[h] = ++d->n;
    return (long) d->n - 1;
}

static long intdict_index(IntDict *d, int64_t v) {
    for (size_t i = 0; i < d->n; i++) {
        if (d->v[i] == v) return (long) i;
    }
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 64;
        int64_t *nv = realloc(d->v, cap * sizeof *nv);
        if (!nv) {
            d->oom = 1;
            return -1;
        }
        d->v = nv;
        d->cap = cap;
    }
    d->v[d->n] = v;
    return (long) d->n++;
}

static long fltdict_index(FltDict *d, double v) {
    for (size_t i = 0; i < d->n; i++) {
        if (d->v[i] == v) return (long) i;
    }
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 16;
        double *nv = realloc(d->v, cap * sizeof *nv);
        if (!nv) {
            d->oom = 1;
            return -1;
        }
        d->v = nv;
        d->cap = cap;
    }
    d->v[d->n] = v;
    return (long) d->n++;
}

static int is_integral(const cJSON *v) {
    const double d = v->valuedouble;
    return isfinite(d) && floor(d) == d && fabs(d) < 9007199254740992.0;
}

/* pass 1: every string, integer and float the tree will refer to */
static void collect(const cJSON *v, Dicts *D) {
    if (!v) return;
    if (cJSON_IsString(v)) {
        if (v->valuestring[0] == 0) return;
        char *esc = escape_dup(v->valuestring);
        if (!esc) {
            D->strings.oom = 1;
            return;
        }
        strdict_index(&D->strings, esc);
        free(esc);
    } else if (cJSON_IsNumber(v)) {
        if (is_integral(v)) intdict_index(&D->ints, (int64_t) v->valuedouble);
        else fltdict_index(&D->floats, v->valuedouble);
    } else if (cJSON_IsArray(v)) {
        for (const cJSON *c = v->child; c; c = c->next) collect(c, D);
    } else if (cJSON_IsObject(v)) {
        for (const cJSON *c = v->child; c; c = c->next) {
            if (c->string && c->string[0]) {
                char *esc = escape_dup(c->string);
                if (!esc) {
                    D->strings.oom = 1;
                    return;
                }
                strdict_index(&D->strings, esc);
                free(esc);
            }
            collect(c, D);
        }
    }
}

/* pass 2: one value as a tree token (the leaf or the nested "@...]" / "$...]") */
static void emit(const cJSON *v, Dicts *D, Out *o);

static void emit_string(const char *s, Dicts *D, Out *o) {
    if (s[0] == 0) {
        out_str(o, "-4");
        return;
    }
    char *esc = escape_dup(s);
    if (!esc) {
        o->oom = 1;
        return;
    }
    /* the dictionary holds every escaped string collect() saw, so the lookup always hits */
    size_t h = fnv1a(esc) & (D->strings.cap - 1);
    long idx = -1;
    while (D->strings.slot[h]) {
        if (!strcmp(D->strings.s[D->strings.slot[h] - 1], esc)) {
            idx = (long) D->strings.slot[h] - 1;
            break;
        }
        h = (h + 1) & (D->strings.cap - 1);
    }
    free(esc);
    if (idx < 0) {
        o->oom = 1;
        return;
    }
    out_base36(o, idx);
}

static void emit_container(const cJSON *v, Dicts *D, Out *o, int object) {
    out_char(o, object ? '$' : '@');
    int first = 1;
    for (const cJSON *c = v->child; c; c = c->next) {
        if (object) {
            /* the key is a string token; "" keys are -4 like "" values */
            if (!first) out_char(o, '|');
            first = 0;
            if (c->string && c->string[0]) emit_string(c->string, D, o);
            else out_str(o, "-4");
            out_char(o, '|');
            emit(c, D, o);
        } else {
            if (!first) out_char(o, '|');
            first = 0;
            emit(c, D, o);
        }
    }
    out_char(o, ']');
}

static void emit(const cJSON *v, Dicts *D, Out *o) {
    if (!v || cJSON_IsNull(v)) {
        out_str(o, "-3");
    } else if (cJSON_IsTrue(v)) {
        out_str(o, "-1");
    } else if (cJSON_IsFalse(v)) {
        out_str(o, "-2");
    } else if (cJSON_IsString(v)) {
        emit_string(v->valuestring, D, o);
    } else if (cJSON_IsNumber(v)) {
        if (is_integral(v)) {
            const long idx = intdict_index(&D->ints, (int64_t) v->valuedouble);
            out_base36(o, (int64_t) D->strings.n + idx);
        } else {
            const long idx = fltdict_index(&D->floats, v->valuedouble);
            out_base36(o, (int64_t) (D->strings.n + D->ints.n) + idx);
        }
    } else if (cJSON_IsArray(v)) {
        emit_container(v, D, o, 0);
    } else if (cJSON_IsObject(v)) {
        emit_container(v, D, o, 1);
    } else {
        out_str(o, "-3");
    }
}

static void dicts_free(Dicts *D) {
    for (size_t i = 0; i < D->strings.n; i++) free(D->strings.s[i]);
    free(D->strings.s);
    free(D->strings.slot);
    free(D->ints.v);
    free(D->floats.v);
}

char *v10_pack_json(const cJSON *json) {
    Dicts D = {{0}, {0}, {0}};
    collect(json, &D);
    Out o = {0};
    if (D.strings.oom || D.ints.oom || D.floats.oom) {
        dicts_free(&D);
        return NULL;
    }
    /* the strings dictionary, joined with "|" (pass 2 uses its slot table) */
    for (size_t i = 0; i < D.strings.n; i++) {
        if (i) out_char(&o, '|');
        out_str(&o, D.strings.s[i]);
    }
    out_char(&o, '^');
    for (size_t i = 0; i < D.ints.n; i++) {
        if (i) out_char(&o, '|');
        out_base36(&o, D.ints.v[i]);
    }
    out_char(&o, '^');
    for (size_t i = 0; i < D.floats.n; i++) {
        if (i) out_char(&o, '|');
        char tmp[40];
        snprintf(tmp, sizeof tmp, "%.17g", D.floats.v[i]);
        out_str(&o, tmp);
    }
    out_char(&o, '^');
    emit(json, &D, &o);
    dicts_free(&D);
    if (o.oom || !o.buf) {
        free(o.buf);
        return NULL;
    }
    return o.buf;
}
