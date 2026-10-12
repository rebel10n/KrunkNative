// KrunkScript value serialisation (doc/v7/krunkscript.md 2.5, 2.8):
//
//   ks_log_format            GAME.log text of one argument: logStringify(sanitize(x)), i.e.
//                            JSON.stringify(x, null, 2) with un-prefixed keys for objects and
//                            arrays, x + "" otherwise. v7 quirks kept: sanitize(null) is {}
//                            (also nested), actions become Symbols (omitted in objects, null
//                            in arrays; a top-level action or a cycle makes v7 throw ->
//                            Internal Error "log").
//   ks_json_length_prefixed  JSON.stringify(data).length on the V_-prefixed value (objects,
//                            arrays, null) or (data + "").length, in UTF-16 units; SIZE_MAX
//                            when JSON.stringify would throw (cycle).
//   ks_serialize/deserialize KsValue binary encoding for NET_PACKET_SCRIPT_MSG: tag byte
//                            0 undefined, 1 null, 2 false, 3 true, 4 f64-LE, 5 str (varint
//                            len + UTF-8), 6 array (varint n + values), 7 obj (varint n +
//                            (key str, value)*). Host objects serialise as objects of their
//                            property values; actions are dropped like JSON (omitted in
//                            objects, null in arrays).
#include "ks_rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Out {
    char *p;
    size_t len, cap;
} Out;

static void out_put(Out *o, const char *s, size_t n)
{
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        while (cap < o->len + n + 1) cap *= 2;
        char *q = realloc(o->p, cap);
        if (!q) abort();
        o->p = q;
        o->cap = cap;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = 0;
}
static void out_str(Out *o, const char *s) { out_put(o, s, strlen(s)); }
static void out_indent(Out *o, int n)
{
    static const char sp[] = "                                ";
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        out_put(o, sp, (size_t)k);
        n -= k;
    }
}

// JSON.stringify string quoting (QuoteJSONString); WTF-8 input.
static void out_json_string(Out *o, const char *s, size_t n)
{
    out_put(o, "\"", 1);
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[8];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\b') esc = "\\b";
        else if (c == '\f') esc = "\\f";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) {
            snprintf(ubuf, sizeof ubuf, "\\u%04x", c);
            esc = ubuf;
        } else if (c == 0xED && i + 2 < n && ((unsigned char)s[i + 1] & 0xE0) == 0xA0) {
            // lone surrogate (3-byte WTF-8) -> \udxxx
            unsigned cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) | ((unsigned char)s[i + 2] & 0x3Fu);
            snprintf(ubuf, sizeof ubuf, "\\u%04x", cp);
            out_put(o, s + i - run, run);
            run = 0;
            out_str(o, ubuf);
            i += 2;
            continue;
        }
        if (esc) {
            out_put(o, s + i - run, run);
            run = 0;
            out_str(o, esc);
        } else {
            run++;
        }
    }
    out_put(o, s + n - run, run);
    out_put(o, "\"", 1);
}

static void out_number_json(Out *o, double d)
{
    char buf[32];
    if (!isfinite(d)) out_put(o, "null", 4);
    else out_put(o, buf, ks_num_format(d, buf));
}

// ---------------------------------------------------------------------------
// GAME.log

typedef struct Ctx {
    KsScript *s;
    const void *stack[1024]; // ancestors (cycle detection)
    int depth;
    int failed;
    size_t visits;
} Ctx;

static int push_ancestor(Ctx *c, const void *p)
{
    for (int i = 0; i < c->depth; i++)
        if (c->stack[i] == p) return 0;
    if (c->depth >= (int)(sizeof c->stack / sizeof c->stack[0])) return 0;
    c->stack[c->depth++] = p;
    return 1;
}

// Emits one property of an object being logged; returns 0 when it is omitted.
static int log_value(Ctx *c, Out *o, KsValue v, int indent, int in_array);

static void log_object_props(Ctx *c, Out *o, KsValue v, int indent)
{
    KsScript *s = c->s;
    int emitted = 0;
    // Collect (key, value) pairs first: host getters may allocate.
    KsStr **keys = NULL;
    KsValue *vals = NULL;
    uint32_t n = 0;
    if (v.tag == KS_OBJ) {
        KsObject *obj = KS_AS_OBJ(v);
        n = obj->count;
        keys = malloc((n ? n : 1) * sizeof *keys);
        vals = malloc((n ? n : 1) * sizeof *vals);
        if (!keys || !vals) abort();
        for (uint32_t i = 0; i < n; i++) {
            keys[i] = obj->props[i].key;
            vals[i] = obj->props[i].val;
        }
    } else {
        KsHostObj *h = KS_AS_HOST(v);
        uint32_t ex = h->expando ? h->expando->count : 0;
        keys = malloc(((uint32_t)h->cls->prop_count + ex + 1) * sizeof *keys);
        vals = malloc(((uint32_t)h->cls->prop_count + ex + 1) * sizeof *vals);
        if (!keys || !vals) abort();
        for (int i = 0; i < h->cls->prop_count; i++) {
            if (h->cls->props[i].method) continue; // functions: omitted by JSON
            KsValue pv;
            if (!ks_get_prop(s, v, h->prop_keys[i], &pv) || s->err) {
                c->failed = 1;
                break;
            }
            keys[n] = h->prop_keys[i];
            vals[n++] = pv;
        }
        for (uint32_t i = 0; i < ex; i++) {
            keys[n] = h->expando->props[i].key;
            vals[n++] = h->expando->props[i].val;
        }
    }
    for (uint32_t i = 0; i < n && !c->failed; i++) {
        KsValue pv = vals[i];
        if (pv.tag == KS_UNDEF || pv.tag == KS_FN) continue;
        out_str(o, emitted ? ",\n" : "{\n");
        out_indent(o, indent + 2);
        out_json_string(o, keys[i]->data, keys[i]->len);
        out_put(o, ": ", 2);
        log_value(c, o, pv, indent + 2, 0);
        emitted = 1;
    }
    free(keys);
    free(vals);
    if (!emitted) out_put(o, "{}", 2);
    else {
        out_put(o, "\n", 1);
        out_indent(o, indent);
        out_put(o, "}", 1);
    }
}

static int log_value(Ctx *c, Out *o, KsValue v, int indent, int in_array)
{
    if (c->failed) return 0;
    if (o->len > KS_MAX_OUTPUT) { // shared sub-objects repeat: bound DAG blow-up (JS: Invalid string length)
        c->failed = 1;
        return 0;
    }
    switch (v.tag) {
    case KS_UNDEF:
    case KS_FN:
        if (in_array) out_put(o, "null", 4);
        return in_array;
    case KS_NULL: out_put(o, "{}", 2); return 1; // sanitize(null) -> {}
    case KS_BOOL: out_str(o, v.as.b ? "true" : "false"); return 1;
    case KS_NUM: out_number_json(o, v.as.num); return 1;
    case KS_STR: out_json_string(o, KS_AS_STR(v)->data, KS_AS_STR(v)->len); return 1;
    case KS_ARR: {
        KsArray *a = KS_AS_ARR(v);
        if (!push_ancestor(c, a)) { c->failed = 1; return 0; }
        if (!a->len) out_put(o, "[]", 2);
        else {
            out_put(o, "[\n", 2);
            for (uint32_t i = 0; i < a->len && !c->failed; i++) {
                if (i) out_put(o, ",\n", 2);
                out_indent(o, indent + 2);
                log_value(c, o, a->items[i], indent + 2, 1);
            }
            out_put(o, "\n", 1);
            out_indent(o, indent);
            out_put(o, "]", 1);
        }
        c->depth--;
        return 1;
    }
    default: // KS_OBJ, KS_HOST
        if (!push_ancestor(c, v.as.ref)) { c->failed = 1; return 0; }
        log_object_props(c, o, v, indent);
        c->depth--;
        return 1;
    }
}

int ks_log_format(KsScript *s, KsValue v, char **out, size_t *out_len)
{
    Out o = {0};
    if (v.tag == KS_FN) { // Symbol("action") + "" throws
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    if (v.tag == KS_OBJ || v.tag == KS_HOST || v.tag == KS_ARR || v.tag == KS_NULL) {
        Ctx *c = calloc(1, sizeof *c);
        if (!c) abort();
        c->s = s;
        log_value(c, &o, v, 0, 0);
        int failed = c->failed;
        free(c);
        if (failed) {
            free(o.p);
            *out = NULL;
            *out_len = 0;
            return 0;
        }
    } else {
        // Strings, numbers, booleans, undefined: x + ""
        char buf[32];
        if (v.tag == KS_STR) out_put(&o, KS_AS_STR(v)->data, KS_AS_STR(v)->len);
        else if (v.tag == KS_NUM) out_put(&o, buf, ks_num_format(v.as.num, buf));
        else if (v.tag == KS_BOOL) out_str(&o, v.as.b ? "true" : "false");
        else out_str(&o, "undefined");
    }
    if (!o.p) out_put(&o, "", 0);
    *out = o.p;
    *out_len = o.len;
    return 1;
}

// ---------------------------------------------------------------------------
// JSON length of the prefixed value (network size rule)

static size_t u16_units(const char *s, size_t n)
{
    size_t u = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80) continue;
        u += c >= 0xF0 ? 2 : 1;
    }
    return u;
}

// JSON-quoted length in UTF-16 units.
static size_t json_string_len(const char *s, size_t n)
{
    size_t len = 2;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') len += 2;
        else if (c < 0x20) len += 6;
        else if (c == 0xED && i + 2 < n && ((unsigned char)s[i + 1] & 0xE0) == 0xA0) { len += 6; i += 2; }
        else if ((c & 0xC0) == 0x80) continue;
        else len += c >= 0xF0 ? 2 : 1;
    }
    return len;
}

static size_t json_len(Ctx *c, KsValue v, int in_array, int *omit)
{
    *omit = 0;
    if (c->failed) return 0;
    if (++c->visits > KS_MAX_OUTPUT) { // DAG blow-up: the result is far above any limit
        c->failed = 1;
        return 0;
    }
    char buf[32];
    switch (v.tag) {
    case KS_UNDEF:
    case KS_FN:
        if (in_array) return 4;
        *omit = 1;
        return 0;
    case KS_NULL: return 4;
    case KS_BOOL: return v.as.b ? 4 : 5;
    case KS_NUM: return isfinite(v.as.num) ? ks_num_format(v.as.num, buf) : 4;
    case KS_STR: return json_string_len(KS_AS_STR(v)->data, KS_AS_STR(v)->len);
    case KS_ARR: {
        KsArray *a = KS_AS_ARR(v);
        if (!push_ancestor(c, a)) { c->failed = 1; return 0; }
        size_t len = 2 + (a->len ? a->len - 1 : 0);
        for (uint32_t i = 0; i < a->len && !c->failed; i++) {
            int om;
            len += json_len(c, a->items[i], 1, &om);
        }
        c->depth--;
        return len;
    }
    default: {
        if (!push_ancestor(c, v.as.ref)) { c->failed = 1; return 0; }
        size_t len = 2, members = 0;
        KsScript *s = c->s;
#define MEMBER(key, val)                                                   \
        do {                                                               \
            int om;                                                        \
            size_t vl = json_len(c, (val), 0, &om);                        \
            if (!om) {                                                     \
                len += json_string_len((key)->data, (key)->len) + 2 + 1 + vl; \
                members++;                                                 \
            }                                                              \
        } while (0)
        // "V_" + key: +2 units per key
        if (v.tag == KS_OBJ) {
            KsObject *o = KS_AS_OBJ(v);
            for (uint32_t i = 0; i < o->count && !c->failed; i++) MEMBER(o->props[i].key, o->props[i].val);
        } else {
            KsHostObj *h = KS_AS_HOST(v);
            for (int i = 0; i < h->cls->prop_count && !c->failed; i++) {
                if (h->cls->props[i].method) continue;
                KsValue pv;
                if (!ks_get_prop(s, v, h->prop_keys[i], &pv)) { c->failed = 1; break; }
                MEMBER(h->prop_keys[i], pv);
            }
            if (h->expando)
                for (uint32_t i = 0; i < h->expando->count && !c->failed; i++)
                    MEMBER(h->expando->props[i].key, h->expando->props[i].val);
        }
#undef MEMBER
        if (members) len += members - 1;
        c->depth--;
        return len;
    }
    }
}

size_t ks_json_length_prefixed(KsScript *s, KsValue v)
{
    if (v.tag == KS_ARR || v.tag == KS_OBJ || v.tag == KS_HOST || v.tag == KS_NULL) {
        Ctx *c = calloc(1, sizeof *c);
        if (!c) abort();
        c->s = s;
        int om;
        size_t len = json_len(c, v, 0, &om);
        int failed = c->failed;
        free(c);
        return failed ? (size_t)-1 : len;
    }
    if (v.tag == KS_STR) return KS_AS_STR(v)->u16len;
    if (v.tag == KS_FN) return 8; // "[action]"
    char buf[32];
    size_t n = ks_to_string(s, v, buf, sizeof buf);
    return u16_units(buf, n);
}

// ---------------------------------------------------------------------------
// Binary encoding

typedef struct Enc {
    uint8_t *out;
    size_t cap, len;
    Ctx *c;
} Enc;

static void enc_bytes(Enc *e, const void *p, size_t n)
{
    if (e->len + n <= e->cap) memcpy(e->out + e->len, p, n);
    e->len += n;
}
static void enc_u8(Enc *e, uint8_t b) { enc_bytes(e, &b, 1); }
static void enc_varint(Enc *e, uint64_t v)
{
    while (v >= 0x80) {
        enc_u8(e, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    enc_u8(e, (uint8_t)v);
}
static void enc_str(Enc *e, const char *p, size_t n)
{
    enc_varint(e, n);
    enc_bytes(e, p, n);
}

static void enc_value(Enc *e, KsValue v);

static void enc_props(Enc *e, KsStr **keys, KsValue *vals, uint32_t n)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++)
        if (vals[i].tag != KS_FN) count++;
    enc_u8(e, 7);
    enc_varint(e, count);
    for (uint32_t i = 0; i < n && !e->c->failed; i++) {
        if (vals[i].tag == KS_FN) continue;
        enc_str(e, keys[i]->data, keys[i]->len);
        enc_value(e, vals[i]);
    }
}

static void enc_value(Enc *e, KsValue v)
{
    if (e->c->failed) return;
    if (e->len > KS_MAX_OUTPUT) {
        e->c->failed = 1;
        return;
    }
    switch (v.tag) {
    case KS_UNDEF: enc_u8(e, 0); return;
    case KS_NULL: enc_u8(e, 1); return;
    case KS_BOOL: enc_u8(e, v.as.b ? 3 : 2); return;
    case KS_NUM: {
        enc_u8(e, 4);
        uint64_t bits;
        memcpy(&bits, &v.as.num, 8);
        uint8_t b[8];
        for (int i = 0; i < 8; i++) b[i] = (uint8_t)(bits >> (8 * i));
        enc_bytes(e, b, 8);
        return;
    }
    case KS_STR: enc_u8(e, 5); enc_str(e, KS_AS_STR(v)->data, KS_AS_STR(v)->len); return;
    case KS_FN: enc_u8(e, 1); return; // only reached inside arrays: JSON null
    case KS_ARR: {
        KsArray *a = KS_AS_ARR(v);
        if (!push_ancestor(e->c, a)) { e->c->failed = 1; return; }
        enc_u8(e, 6);
        enc_varint(e, a->len);
        for (uint32_t i = 0; i < a->len && !e->c->failed; i++) enc_value(e, a->items[i]);
        e->c->depth--;
        return;
    }
    case KS_OBJ: {
        KsObject *o = KS_AS_OBJ(v);
        if (!push_ancestor(e->c, o)) { e->c->failed = 1; return; }
        KsStr **keys = malloc((o->count + 1) * sizeof *keys);
        KsValue *vals = malloc((o->count + 1) * sizeof *vals);
        if (!keys || !vals) abort();
        for (uint32_t i = 0; i < o->count; i++) { keys[i] = o->props[i].key; vals[i] = o->props[i].val; }
        enc_props(e, keys, vals, o->count);
        free(keys);
        free(vals);
        e->c->depth--;
        return;
    }
    case KS_HOST: {
        KsHostObj *h = KS_AS_HOST(v);
        if (!push_ancestor(e->c, h)) { e->c->failed = 1; return; }
        uint32_t ex = h->expando ? h->expando->count : 0, n = 0;
        KsStr **keys = malloc(((uint32_t)h->cls->prop_count + ex + 1) * sizeof *keys);
        KsValue *vals = malloc(((uint32_t)h->cls->prop_count + ex + 1) * sizeof *vals);
        if (!keys || !vals) abort();
        for (int i = 0; i < h->cls->prop_count; i++) {
            if (h->cls->props[i].method) continue;
            KsValue pv;
            if (!ks_get_prop(e->c->s, v, h->prop_keys[i], &pv)) { e->c->failed = 1; break; }
            keys[n] = h->prop_keys[i];
            vals[n++] = pv;
        }
        for (uint32_t i = 0; i < ex; i++) { keys[n] = h->expando->props[i].key; vals[n++] = h->expando->props[i].val; }
        if (!e->c->failed) enc_props(e, keys, vals, n);
        free(keys);
        free(vals);
        e->c->depth--;
        return;
    }
    }
}

size_t ks_serialize(KsScript *s, KsValue v, uint8_t *out, size_t cap)
{
    Ctx *c = calloc(1, sizeof *c);
    if (!c) abort();
    c->s = s;
    Enc e = {out, out ? cap : 0, 0, c};
    enc_value(&e, v.tag == KS_FN ? ks_null() : v);
    int failed = c->failed;
    free(c);
    return failed ? 0 : e.len;
}

typedef struct Dec {
    KsScript *s;
    const uint8_t *in;
    size_t len, pos;
    int ok;
    int depth;
} Dec;

static int dec_varint(Dec *d, uint64_t *v)
{
    uint64_t r = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (d->pos >= d->len) return 0;
        uint8_t b = d->in[d->pos++];
        r |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *v = r;
            return 1;
        }
    }
    return 0;
}

static KsStr *dec_str(Dec *d, int intern)
{
    uint64_t n;
    if (!dec_varint(d, &n) || n > d->len - d->pos) { d->ok = 0; return d->s->s_empty; }
    const char *p = (const char *)d->in + d->pos;
    d->pos += (size_t)n;
    return intern ? ks_str_intern(d->s, p, (size_t)n) : ks_str_new(d->s, p, (size_t)n);
}

static KsValue dec_value(Dec *d)
{
    if (!d->ok || d->pos >= d->len || d->depth > 256 || d->s->err) { d->ok = 0; return ks_undef(); }
    uint8_t tag = d->in[d->pos++];
    switch (tag) {
    case 0: return ks_undef();
    case 1: return ks_null();
    case 2: return ks_bool(0);
    case 3: return ks_bool(1);
    case 4: {
        if (d->len - d->pos < 8) { d->ok = 0; return ks_undef(); }
        uint64_t bits = 0;
        for (int i = 0; i < 8; i++) bits |= (uint64_t)d->in[d->pos + i] << (8 * i);
        d->pos += 8;
        double x;
        memcpy(&x, &bits, 8);
        return ks_num(x);
    }
    case 5: return ks_str_value(dec_str(d, 0));
    case 6: {
        uint64_t n;
        if (!dec_varint(d, &n) || n > d->len - d->pos || n > KS_MAX_ARR_LEN) { d->ok = 0; return ks_undef(); }
        KsArray *a = ks_array_new(d->s, (uint32_t)n);
        d->depth++;
        for (uint64_t i = 0; i < n && d->ok; i++) a->items[a->len++] = dec_value(d);
        d->depth--;
        return ks_ref_value(KS_ARR, a);
    }
    case 7: {
        uint64_t n;
        if (!dec_varint(d, &n) || n > d->len - d->pos) { d->ok = 0; return ks_undef(); }
        KsObject *o = ks_object_new(d->s);
        d->depth++;
        for (uint64_t i = 0; i < n && d->ok; i++) {
            KsStr *k = dec_str(d, 1);
            KsValue v = dec_value(d);
            if (d->ok && !ks_object_put(d->s, o, k, v)) d->ok = 0;
        }
        d->depth--;
        return ks_ref_value(KS_OBJ, o);
    }
    default:
        d->ok = 0;
        return ks_undef();
    }
}

KsValue ks_deserialize(KsScript *s, const uint8_t *in, size_t len, int *ok)
{
    Dec d = {s, in, len, 0, 1, 0};
    KsValue v = in && len ? dec_value(&d) : ks_undef();
    if (!in || !len || d.pos != len) d.ok = 0;
    if (ok) *ok = d.ok;
    return d.ok ? v : ks_undef();
}
