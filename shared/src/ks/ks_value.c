// KrunkScript runtime: per-script heap (precise mark-and-sweep), WTF-8 strings with UTF-16
// semantics, insertion-ordered objects, arrays, action values, host-object wrappers and the
// JS conversions every other module uses (doc/v7/krunkscript.md 1.4, 1.10-1.12, 6.4).
//
// GC rules: allocation never collects. ks_gc_maybe() runs only at VM safe points (host
// entry, calls, loop back-edges) where every live value is reachable from the roots:
// globals, the VM stack, constant strings, cached action values, pinned handles and the
// host-object cache (whose wrappers carry script-added properties across fetches).
#include "ks_rt.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Raw memory

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "KrunkScript: out of memory (%zu bytes)\n", n);
        abort();
    }
    return q;
}

void ks_heap_account(KsScript *s, long delta)
{
    if (delta < 0 && (size_t)(-delta) > s->bytes) s->bytes = 0;
    else s->bytes = (size_t)((long)s->bytes + delta);
    if (s->bytes > 2 * KS_HEAP_LIMIT && !s->oom) {
        s->oom = 1;
        s->failed = 1;
        ks_raise(s, "Out of memory.");
    }
}

void *ks_heap_alloc(KsScript *s, size_t size, uint8_t type)
{
    KsObj *o = xrealloc(NULL, size);
    memset(o, 0, size);
    o->type = type;
    o->next = s->objects;
    s->objects = o;
    ks_heap_account(s, (long)size);
    return o;
}

static size_t obj_size(const KsObj *o)
{
    switch (o->type) {
    case KS_GC_STR: return sizeof(KsStr) + ((const KsStr *)o)->len + 1;
    case KS_GC_ARR: return sizeof(KsArray) + ((const KsArray *)o)->cap * sizeof(KsValue);
    case KS_GC_OBJ: {
        const KsObject *x = (const KsObject *)o;
        return sizeof(KsObject) + x->cap * sizeof(KsProp) + x->index_cap * sizeof(int32_t);
    }
    case KS_GC_FN: return sizeof(KsFn);
    case KS_GC_HOST: return sizeof(KsHostObj);
    }
    return 0;
}

static void obj_free(KsObj *o)
{
    switch (o->type) {
    case KS_GC_ARR: free(((KsArray *)o)->items); break;
    case KS_GC_OBJ: free(((KsObject *)o)->props); free(((KsObject *)o)->index); break;
    default: break;
    }
    free(o);
}

// ---------------------------------------------------------------------------
// Errors

void ks_raise(KsScript *s, const char *fmt, ...)
{
    if (s->err) return;
    s->err = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err_msg, sizeof s->err_msg, fmt, ap);
    va_end(ap);
}

void ks_raise_at(KsScript *s, int line, int col, const char *msg)
{
    ks_raise(s, "%d:%d %s", line, col, msg);
}

// ---------------------------------------------------------------------------
// Strings

uint32_t ks_str_hash(KsStr *str)
{
    if (str->hash) return str->hash;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < str->len; i++) h = (h ^ (uint8_t)str->data[i]) * 16777619u;
    if (!h) h = 1;
    str->hash = h;
    return h;
}

static uint32_t bytes_hash(const char *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
    return h ? h : 1;
}

KsStr *ks_str_new_raw(KsScript *s, const char *wtf8, size_t len, uint32_t u16len, int ascii)
{
    KsStr *str = ks_heap_alloc(s, sizeof(KsStr) + len + 1, KS_GC_STR);
    str->len = (uint32_t)len;
    str->u16len = u16len;
    str->ascii = (uint8_t)ascii;
    if (len) memcpy(str->data, wtf8, len);
    str->data[len] = 0;
    return str;
}

// Decodes one WTF-8 sequence at p[i]; returns the code point (surrogates allowed) and its
// byte length, or -1 when invalid.
static int32_t wtf8_decode(const unsigned char *p, size_t len, size_t i, int *n)
{
    unsigned char c = p[i];
    if (c < 0x80) { *n = 1; return c; }
    if (c < 0xC2) return -1;
    if (c < 0xE0) {
        if (i + 1 >= len || (p[i + 1] & 0xC0) != 0x80) return -1;
        *n = 2;
        return ((c & 0x1F) << 6) | (p[i + 1] & 0x3F);
    }
    if (c < 0xF0) {
        if (i + 2 >= len || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return -1;
        int32_t cp = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
        if (cp < 0x800) return -1;
        *n = 3;
        return cp;
    }
    if (c < 0xF5) {
        if (i + 3 >= len || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80 ||
            (p[i + 3] & 0xC0) != 0x80)
            return -1;
        int32_t cp = ((c & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) | ((p[i + 2] & 0x3F) << 6) |
                     (p[i + 3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return -1;
        *n = 4;
        return cp;
    }
    return -1;
}

static size_t wtf8_encode(int32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int too_long(KsScript *s, uint64_t units)
{
    if (units > KS_MAX_STR_UNITS) {
        ks_raise(s, "Invalid string length");
        return 1;
    }
    return 0;
}

KsStr *ks_str_new(KsScript *s, const char *utf8, size_t len)
{
    const unsigned char *p = (const unsigned char *)utf8;
    size_t i = 0;
    while (i < len && p[i] < 0x80) i++;
    if (i == len) {
        if (too_long(s, len)) return s->s_empty;
        return ks_str_new_raw(s, utf8, len, (uint32_t)len, 1);
    }
    // Validate / canonicalise: invalid bytes -> U+FFFD, surrogate pairs -> 4-byte form.
    char *out = xrealloc(NULL, len * 3 + 4);
    size_t o = 0;
    uint64_t units = 0;
    memcpy(out, utf8, i);
    o = units = i;
    while (i < len) {
        int n = 1;
        int32_t cp = wtf8_decode(p, len, i, &n);
        if (cp < 0) { cp = 0xFFFD; n = 1; }
        if (cp >= 0xD800 && cp <= 0xDBFF && i + n < len) {
            int n2;
            int32_t lo = wtf8_decode(p, len, i + n, &n2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                n += n2;
            }
        }
        o += wtf8_encode(cp, out + o);
        units += cp >= 0x10000 ? 2 : 1;
        i += (size_t)n;
    }
    KsStr *str = too_long(s, units) ? s->s_empty : ks_str_new_raw(s, out, o, (uint32_t)units, 0);
    free(out);
    return str;
}

// Lone high surrogate at the end / lone low surrogate at the start (3-byte WTF-8 forms).
static int ends_with_high(const KsStr *a)
{
    return a->len >= 3 && (uint8_t)a->data[a->len - 3] == 0xED && ((uint8_t)a->data[a->len - 2] & 0xF0) == 0xA0;
}
static int starts_with_low(const KsStr *b)
{
    return b->len >= 3 && (uint8_t)b->data[0] == 0xED && ((uint8_t)b->data[1] & 0xF0) == 0xB0;
}

KsStr *ks_str_concat(KsScript *s, KsStr *a, KsStr *b)
{
    if (!b->len) return a;
    if (!a->len) return b;
    uint64_t units = (uint64_t)a->u16len + b->u16len;
    if (too_long(s, units)) return s->s_empty;
    if (ends_with_high(a) && starts_with_low(b)) {
        int n;
        int32_t hi = wtf8_decode((const unsigned char *)a->data, a->len, a->len - 3, &n);
        int32_t lo = wtf8_decode((const unsigned char *)b->data, b->len, 0, &n);
        int32_t cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
        size_t len = a->len - 3 + 4 + b->len - 3;
        char *buf = xrealloc(NULL, len);
        memcpy(buf, a->data, a->len - 3);
        wtf8_encode(cp, buf + a->len - 3);
        memcpy(buf + a->len + 1, b->data + 3, b->len - 3);
        KsStr *r = ks_str_new_raw(s, buf, len, (uint32_t)units, 0);
        free(buf);
        return r;
    }
    size_t len = (size_t)a->len + b->len;
    KsStr *str = ks_heap_alloc(s, sizeof(KsStr) + len + 1, KS_GC_STR);
    str->len = (uint32_t)len;
    str->u16len = (uint32_t)units;
    str->ascii = a->ascii && b->ascii;
    memcpy(str->data, a->data, a->len);
    memcpy(str->data + a->len, b->data, b->len);
    str->data[len] = 0;
    return str;
}

// Walks to UTF-16 unit `index`: returns the byte offset of the sequence containing it and
// sets *second when the unit is the low half of a 4-byte sequence.
static uint32_t unit_to_byte(const KsStr *cstr, uint32_t index, int *second)
{
    *second = 0;
    if (cstr->ascii) return index;
    KsStr *str = (KsStr *)cstr; // the position cache is not part of the value
    const unsigned char *p = (const unsigned char *)str->data;
    uint32_t i = 0, u = 0;
    if (index >= str->cache_unit && str->cache_byte <= str->len) {
        i = str->cache_byte;
        u = str->cache_unit;
    }
    while (i < str->len) {
        int n = 1;
        int32_t cp = wtf8_decode(p, str->len, i, &n);
        uint32_t w = cp >= 0x10000 ? 2 : 1;
        if (index < u + w) {
            *second = index > u;
            str->cache_unit = u;
            str->cache_byte = i;
            return i;
        }
        u += w;
        i += (uint32_t)n;
    }
    return str->len;
}

int ks_str_unit_at(const KsStr *str, uint32_t index, uint16_t *unit)
{
    if (index >= str->u16len) return 0;
    if (str->ascii) { *unit = (uint8_t)str->data[index]; return 1; }
    int second;
    uint32_t b = unit_to_byte(str, index, &second);
    int n;
    int32_t cp = wtf8_decode((const unsigned char *)str->data, str->len, b, &n);
    if (cp >= 0x10000) {
        cp -= 0x10000;
        *unit = (uint16_t)(second ? 0xDC00 + (cp & 0x3FF) : 0xD800 + (cp >> 10));
    } else {
        *unit = (uint16_t)cp;
    }
    return 1;
}

KsStr *ks_str_substring(KsScript *s, KsStr *str, uint32_t from, uint32_t to)
{
    if (to > str->u16len) to = str->u16len;
    if (from >= to) return s->s_empty;
    if (from == 0 && to == str->u16len) return str;
    if (str->ascii) return ks_str_new_raw(s, str->data + from, to - from, to - from, 1);
    char *out = xrealloc(NULL, (size_t)(to - from) * 3 + 4);
    size_t o = 0;
    for (uint32_t u = from; u < to;) {
        int second;
        uint32_t b = unit_to_byte(str, u, &second);
        int n;
        int32_t cp = wtf8_decode((const unsigned char *)str->data, str->len, b, &n);
        if (cp >= 0x10000) {
            int32_t c = cp - 0x10000;
            if (second) { o += wtf8_encode(0xDC00 + (c & 0x3FF), out + o); u++; }
            else if (u + 1 >= to) { o += wtf8_encode(0xD800 + (c >> 10), out + o); u++; }
            else { o += wtf8_encode(cp, out + o); u += 2; }
        } else {
            o += wtf8_encode(cp, out + o);
            u++;
        }
        // Fast-forward the common case: continue with sequential decoding.
        while (u < to) {
            b += (uint32_t)n;
            if (b >= str->len) break;
            cp = wtf8_decode((const unsigned char *)str->data, str->len, b, &n);
            if (cp >= 0x10000) {
                if (u + 1 >= to) { o += wtf8_encode(0xD800 + ((cp - 0x10000) >> 10), out + o); u++; break; }
                o += wtf8_encode(cp, out + o);
                u += 2;
            } else {
                o += wtf8_encode(cp, out + o);
                u++;
            }
        }
    }
    KsStr *r = ks_str_new_raw(s, out, o, to - from, 0);
    int ascii = 1;
    for (size_t i = 0; i < o && ascii; i++) ascii = (uint8_t)out[i] < 0x80;
    r->ascii = (uint8_t)ascii;
    free(out);
    return r;
}

int ks_str_equal(const KsStr *a, const KsStr *b)
{
    return a == b || (a->len == b->len && memcmp(a->data, b->data, a->len) == 0);
}

// Next UTF-16 unit while iterating; *pending holds the low half of a pair.
static int next_unit(const KsStr *str, uint32_t *i, int32_t *pending, uint16_t *unit)
{
    if (*pending >= 0) { *unit = (uint16_t)*pending; *pending = -1; return 1; }
    if (*i >= str->len) return 0;
    int n;
    int32_t cp = wtf8_decode((const unsigned char *)str->data, str->len, *i, &n);
    *i += (uint32_t)n;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        *unit = (uint16_t)(0xD800 + (cp >> 10));
        *pending = 0xDC00 + (cp & 0x3FF);
    } else {
        *unit = (uint16_t)cp;
    }
    return 1;
}

int ks_str_compare(const KsStr *a, const KsStr *b)
{
    if (a->ascii && b->ascii) {
        uint32_t n = a->len < b->len ? a->len : b->len;
        int c = memcmp(a->data, b->data, n);
        if (c) return c < 0 ? -1 : 1;
        return a->len < b->len ? -1 : a->len > b->len;
    }
    uint32_t ia = 0, ib = 0;
    int32_t pa = -1, pb = -1;
    for (;;) {
        uint16_t ua, ub;
        int ha = next_unit(a, &ia, &pa, &ua), hb = next_unit(b, &ib, &pb, &ub);
        if (!ha || !hb) return ha - hb;
        if (ua != ub) return ua < ub ? -1 : 1;
    }
}

long ks_str_index_of(const KsStr *hay, const KsStr *needle, uint32_t from)
{
    uint32_t m = needle->len, n = hay->len;
    if (m == 0) return from <= n ? (long)from : -1;
    if (m > n || from > n - m) return -1;
    if (m == 1) {
        const char *hit = memchr(hay->data + from, needle->data[0], n - from);
        return hit ? (long)(hit - hay->data) : -1;
    }
    // Knuth-Morris-Pratt: linear in |hay| + |needle| (scripts control both).
    uint32_t *fail = xrealloc(NULL, m * sizeof(uint32_t));
    fail[0] = 0;
    for (uint32_t i = 1, k = 0; i < m; i++) {
        while (k && needle->data[i] != needle->data[k]) k = fail[k - 1];
        if (needle->data[i] == needle->data[k]) k++;
        fail[i] = k;
    }
    long found = -1;
    for (uint32_t i = from, k = 0; i < n; i++) {
        while (k && hay->data[i] != needle->data[k]) k = fail[k - 1];
        if (hay->data[i] == needle->data[k]) k++;
        if (k == m) {
            found = (long)(i + 1 - m);
            break;
        }
    }
    free(fail);
    return found;
}

uint32_t ks_str_u16_offset(const KsStr *str, uint32_t byte_off)
{
    if (str->ascii) return byte_off;
    uint32_t u = 0;
    for (uint32_t i = 0; i < byte_off && i < str->len;) {
        int n;
        int32_t cp = wtf8_decode((const unsigned char *)str->data, str->len, i, &n);
        u += cp >= 0x10000 ? 2 : 1;
        i += (uint32_t)n;
    }
    return u;
}

// ---------------------------------------------------------------------------
// Interning (weak table, rebuilt after each sweep)

static void intern_insert(KsScript *s, KsStr *str)
{
    uint32_t mask = s->intern_cap - 1;
    uint32_t h = ks_str_hash(str) & mask;
    while (s->intern[h]) h = (h + 1) & mask;
    s->intern[h] = str;
    s->intern_count++;
}

static void intern_grow(KsScript *s, uint32_t cap)
{
    KsStr **old = s->intern;
    uint32_t old_cap = s->intern_cap;
    s->intern = xrealloc(NULL, cap * sizeof(KsStr *));
    memset(s->intern, 0, cap * sizeof(KsStr *));
    s->intern_cap = cap;
    s->intern_count = 0;
    for (uint32_t i = 0; i < old_cap; i++)
        if (old[i]) intern_insert(s, old[i]);
    free(old);
}

static KsStr *intern_lookup(KsScript *s, const char *p, size_t len, uint32_t h)
{
    if (!s->intern_cap) return NULL;
    uint32_t mask = s->intern_cap - 1;
    for (uint32_t i = h & mask; s->intern[i]; i = (i + 1) & mask) {
        KsStr *c = s->intern[i];
        if (c->hash == h && c->len == len && memcmp(c->data, p, len) == 0) return c;
    }
    return NULL;
}

KsStr *ks_str_intern_str(KsScript *s, KsStr *str)
{
    if (str->interned) return str;
    uint32_t h = ks_str_hash(str);
    KsStr *found = intern_lookup(s, str->data, str->len, h);
    if (found) return found;
    if ((s->intern_count + 1) * 2 > s->intern_cap) intern_grow(s, s->intern_cap ? s->intern_cap * 2 : 256);
    str->interned = 1;
    intern_insert(s, str);
    return str;
}

KsStr *ks_str_intern(KsScript *s, const char *utf8, size_t len)
{
    uint32_t h = bytes_hash(utf8, len);
    KsStr *found = intern_lookup(s, utf8, len, h);
    if (found) return found;
    return ks_str_intern_str(s, ks_str_new(s, utf8, len));
}

// ---------------------------------------------------------------------------
// Objects

#define KS_INDEX_MIN 9

KsObject *ks_object_new(KsScript *s)
{
    return ks_heap_alloc(s, sizeof(KsObject), KS_GC_OBJ);
}

static void object_reindex(KsScript *s, KsObject *o)
{
    if (o->count < KS_INDEX_MIN) {
        if (o->index) {
            ks_heap_account(s, -(long)(o->index_cap * sizeof(int32_t)));
            free(o->index);
            o->index = NULL;
            o->index_cap = 0;
        }
        return;
    }
    uint32_t cap = 16;
    while (cap < o->count * 2) cap *= 2;
    if (cap != o->index_cap) {
        ks_heap_account(s, ((long)cap - (long)o->index_cap) * (long)sizeof(int32_t));
        o->index = xrealloc(o->index, cap * sizeof(int32_t));
        o->index_cap = cap;
    }
    memset(o->index, 0xFF, cap * sizeof(int32_t));
    uint32_t mask = cap - 1;
    for (uint32_t i = 0; i < o->count; i++) {
        uint32_t h = o->props[i].key->hash & mask;
        while (o->index[h] >= 0) h = (h + 1) & mask;
        o->index[h] = (int32_t)i;
    }
}

KsValue *ks_object_find(KsObject *o, const KsStr *key)
{
    if (!o->index) {
        for (uint32_t i = 0; i < o->count; i++)
            if (o->props[i].key == key) return &o->props[i].val;
        return NULL;
    }
    uint32_t mask = o->index_cap - 1;
    for (uint32_t h = key->hash & mask; o->index[h] >= 0; h = (h + 1) & mask)
        if (o->props[o->index[h]].key == key) return &o->props[o->index[h]].val;
    return NULL;
}

int ks_object_put(KsScript *s, KsObject *o, KsStr *key, KsValue v)
{
    KsValue *slot = ks_object_find(o, key);
    if (slot) { *slot = v; return 1; }
    if (o->count >= KS_MAX_PROPS) {
        ks_raise(s, "Too many properties.");
        return 0;
    }
    if (o->count == o->cap) {
        uint32_t cap = o->cap ? o->cap * 2 : 4;
        ks_heap_account(s, ((long)cap - (long)o->cap) * (long)sizeof(KsProp));
        o->props = xrealloc(o->props, cap * sizeof(KsProp));
        o->cap = cap;
    }
    o->props[o->count].key = key;
    o->props[o->count].val = v;
    o->count++;
    if (o->count >= KS_INDEX_MIN) {
        if (!o->index || o->count * 2 > o->index_cap) {
            object_reindex(s, o);
        } else {
            uint32_t mask = o->index_cap - 1, h = key->hash & mask;
            while (o->index[h] >= 0) h = (h + 1) & mask;
            o->index[h] = (int32_t)(o->count - 1);
        }
    }
    return 1;
}

int ks_object_delete(KsScript *s, KsObject *o, const KsStr *key)
{
    KsValue *slot = ks_object_find(o, key);
    if (!slot) return 0;
    uint32_t i = (uint32_t)((KsProp *)((char *)slot - offsetof(KsProp, val)) - o->props);
    memmove(o->props + i, o->props + i + 1, (o->count - i - 1) * sizeof(KsProp));
    o->count--;
    if (o->index) object_reindex(s, o);
    return 1;
}

// ---------------------------------------------------------------------------
// Arrays

KsArray *ks_array_new(KsScript *s, uint32_t cap)
{
    KsArray *a = ks_heap_alloc(s, sizeof(KsArray), KS_GC_ARR);
    if (cap) {
        a->items = xrealloc(NULL, cap * sizeof(KsValue));
        a->cap = cap;
        ks_heap_account(s, (long)(cap * sizeof(KsValue)));
    }
    return a;
}

int ks_array_push(KsScript *s, KsArray *a, KsValue v)
{
    if (a->len >= KS_MAX_ARR_LEN) {
        ks_raise(s, "Invalid array length");
        return 0;
    }
    if (a->len == a->cap) {
        uint32_t cap = a->cap ? a->cap * 2 : 4;
        ks_heap_account(s, ((long)cap - (long)a->cap) * (long)sizeof(KsValue));
        a->items = xrealloc(a->items, cap * sizeof(KsValue));
        a->cap = cap;
    }
    a->items[a->len++] = v;
    return 1;
}

void ks_array_splice1(KsScript *s, KsArray *a, uint32_t at)
{
    (void)s;
    if (at >= a->len) return;
    memmove(a->items + at, a->items + at + 1, (a->len - at - 1) * sizeof(KsValue));
    a->len--;
}

// ---------------------------------------------------------------------------
// Functions

KsFn *ks_fn_action(KsScript *s, uint32_t action)
{
    if (s->action_fns && action < s->prog->action_count) {
        if (!s->action_fns[action]) {
            KsFn *f = ks_heap_alloc(s, sizeof(KsFn), KS_GC_FN);
            f->kind = KS_FN_ACTION;
            f->index = (int32_t)action;
            s->action_fns[action] = f;
        }
        return s->action_fns[action];
    }
    KsFn *f = ks_heap_alloc(s, sizeof(KsFn), KS_GC_FN);
    f->kind = KS_FN_ACTION;
    f->index = (int32_t)action;
    return f;
}

KsFn *ks_fn_api(KsScript *s, int32_t api_id)
{
    KsFn *f = ks_heap_alloc(s, sizeof(KsFn), KS_GC_FN);
    f->kind = KS_FN_API;
    f->index = api_id;
    return f;
}

KsFn *ks_fn_method(KsScript *s, KsHostObj *self, const KsHostProp *method)
{
    KsFn *f = ks_heap_alloc(s, sizeof(KsFn), KS_GC_FN);
    f->kind = KS_FN_METHOD;
    f->self = self;
    f->method = method;
    return f;
}

// ---------------------------------------------------------------------------
// Host objects

KsStr **ks_class_keys(KsScript *s, const KsHostClass *cls)
{
    for (uint32_t i = 0; i < s->class_key_count; i++)
        if (s->class_keys[i].cls == cls) return s->class_keys[i].keys;
    s->class_keys = xrealloc(s->class_keys, (s->class_key_count + 1) * sizeof(KsClassKeys));
    KsStr **keys = xrealloc(NULL, (size_t)(cls->prop_count ? cls->prop_count : 1) * sizeof(KsStr *));
    for (int i = 0; i < cls->prop_count; i++)
        keys[i] = ks_str_intern(s, cls->props[i].name, strlen(cls->props[i].name));
    s->class_keys[s->class_key_count].cls = cls;
    s->class_keys[s->class_key_count].keys = keys;
    s->class_key_count++;
    return keys;
}

const KsHostProp *ks_host_find_prop(KsHostObj *h, const KsStr *key)
{
    for (int i = 0; i < h->cls->prop_count; i++)
        if (h->prop_keys[i] == key) return &h->cls->props[i];
    return NULL;
}

static uint32_t hcache_hash(const KsHostClass *cls, uint32_t id)
{
    uint64_t x = (uint64_t)(uintptr_t)cls * 0x9E3779B97F4A7C15ull ^ (uint64_t)id * 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(x >> 32) ^ (uint32_t)x;
}

static void hcache_insert(KsScript *s, KsHostObj *obj)
{
    uint32_t mask = s->hcache_cap - 1;
    uint32_t h = hcache_hash(obj->cls, obj->native_id) & mask;
    while (s->hcache[h].obj) h = (h + 1) & mask;
    s->hcache[h].cls = obj->cls;
    s->hcache[h].native_id = obj->native_id;
    s->hcache[h].obj = obj;
    s->hcache_count++;
}

KsValue ks_host_object(KsScript *s, const KsHostClass *cls, void *native, uint32_t native_id)
{
    if (s->hcache_cap) {
        uint32_t mask = s->hcache_cap - 1;
        for (uint32_t h = hcache_hash(cls, native_id) & mask; s->hcache[h].obj; h = (h + 1) & mask) {
            if (s->hcache[h].cls == cls && s->hcache[h].native_id == native_id) {
                s->hcache[h].obj->native = native;
                return ks_ref_value(KS_HOST, s->hcache[h].obj);
            }
        }
    }
    if ((s->hcache_count + 1) * 2 > s->hcache_cap) {
        KsHostCacheEntry *old = s->hcache;
        uint32_t old_cap = s->hcache_cap;
        s->hcache_cap = old_cap ? old_cap * 2 : 64;
        s->hcache = xrealloc(NULL, s->hcache_cap * sizeof(KsHostCacheEntry));
        memset(s->hcache, 0, s->hcache_cap * sizeof(KsHostCacheEntry));
        s->hcache_count = 0;
        for (uint32_t i = 0; i < old_cap; i++)
            if (old[i].obj) hcache_insert(s, old[i].obj);
        free(old);
    }
    KsHostObj *obj = ks_heap_alloc(s, sizeof(KsHostObj), KS_GC_HOST);
    obj->cls = cls;
    obj->native = native;
    obj->native_id = native_id;
    obj->prop_keys = ks_class_keys(s, cls);
    hcache_insert(s, obj);
    return ks_ref_value(KS_HOST, obj);
}

void ks_host_forget(KsScript *s, const KsHostClass *cls, uint32_t native_id)
{
    if (!s->hcache_cap) return;
    uint32_t mask = s->hcache_cap - 1;
    uint32_t h = hcache_hash(cls, native_id) & mask;
    for (; s->hcache[h].obj; h = (h + 1) & mask)
        if (s->hcache[h].cls == cls && s->hcache[h].native_id == native_id) break;
    if (!s->hcache[h].obj) return;
    s->hcache[h].obj->native = NULL;
    // backward-shift deletion (linear probing)
    uint32_t i = h;
    for (;;) {
        s->hcache[i].obj = NULL;
        uint32_t j = i;
        for (;;) {
            j = (j + 1) & mask;
            if (!s->hcache[j].obj) goto done;
            uint32_t k = hcache_hash(s->hcache[j].cls, s->hcache[j].native_id) & mask;
            if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) break;
        }
        s->hcache[i] = s->hcache[j];
        i = j;
    }
done:
    s->hcache_count--;
}

// ---------------------------------------------------------------------------
// Generic property access

static const char *recv_kind(KsValue v) { return v.tag == KS_NULL ? "null" : "undefined"; }

int ks_get_prop(KsScript *s, KsValue recv, KsStr *key, KsValue *out)
{
    *out = ks_undef();
    switch (recv.tag) {
    case KS_UNDEF:
    case KS_NULL:
        ks_raise(s, "Cannot read properties of %s (reading '%s')", recv_kind(recv), key->data);
        return 0;
    case KS_OBJ: {
        KsValue *v = ks_object_find(KS_AS_OBJ(recv), key);
        if (v) *out = *v;
        return 1;
    }
    case KS_HOST: {
        KsHostObj *h = KS_AS_HOST(recv);
        const KsHostProp *p = ks_host_find_prop(h, key);
        if (p) {
            if (p->method) *out = ks_ref_value(KS_FN, ks_fn_method(s, h, p));
            else if (p->get && h->native) {
                s->native_depth++;
                *out = p->get(s, h->native, h->native_id);
                s->native_depth--;
            }
            return !s->err;
        }
        if (h->expando) {
            KsValue *v = ks_object_find(h->expando, key);
            if (v) *out = *v;
        }
        return 1;
    }
    default:
        return 1;
    }
}

static void describe_primitive(KsScript *s, KsValue v, char *buf, size_t n)
{
    KsStr *str = ks_to_str(s, v);
    snprintf(buf, n, "%s", str->data);
}

int ks_set_prop(KsScript *s, KsValue recv, KsStr *key, KsValue v)
{
    switch (recv.tag) {
    case KS_UNDEF:
    case KS_NULL:
        ks_raise(s, "Cannot set properties of %s (setting '%s')", recv_kind(recv), key->data);
        return 0;
    case KS_NUM:
    case KS_STR:
    case KS_BOOL: {
        char buf[128];
        describe_primitive(s, recv, buf, sizeof buf);
        const char *kind = recv.tag == KS_NUM ? "number" : recv.tag == KS_STR ? "string" : "boolean";
        ks_raise(s, "Cannot create property '%s' on %s '%s'", key->data, kind, buf);
        return 0;
    }
    case KS_OBJ:
        return ks_object_put(s, KS_AS_OBJ(recv), key, v);
    case KS_HOST: {
        KsHostObj *h = KS_AS_HOST(recv);
        const KsHostProp *p = ks_host_find_prop(h, key);
        if (p) {
            // Built-in host properties: setters coerce; read-only, sub-objects and methods
            // are non-writable and silently keep their value (doc 1.10).
            if (p->set && !p->method && !p->is_subobject && h->native) {
                s->native_depth++;
                p->set(s, h->native, h->native_id, v);
                s->native_depth--;
            }
            return !s->err;
        }
        if (!h->expando) h->expando = ks_object_new(s);
        return ks_object_put(s, h->expando, key, v);
    }
    default:
        return 1; // arrays/actions: JS would add a property; KN ignores it
    }
}

int ks_has_own(KsScript *s, KsValue recv, KsStr *key, int *out)
{
    *out = 0;
    switch (recv.tag) {
    case KS_UNDEF:
    case KS_NULL:
        ks_raise(s, "Cannot read properties of %s (reading 'hasOwnProperty')", recv_kind(recv));
        return 0;
    case KS_OBJ:
        *out = ks_object_find(KS_AS_OBJ(recv), key) != NULL;
        return 1;
    case KS_HOST: {
        KsHostObj *h = KS_AS_HOST(recv);
        *out = ks_host_find_prop(h, key) != NULL || (h->expando && ks_object_find(h->expando, key));
        return 1;
    }
    default:
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Conversions

int ks_truthy(KsValue v)
{
    switch (v.tag) {
    case KS_UNDEF:
    case KS_NULL: return 0;
    case KS_BOOL: return v.as.b != 0;
    case KS_NUM: return !(v.as.num == 0 || isnan(v.as.num));
    case KS_STR: return KS_AS_STR(v)->len != 0;
    default: return 1;
    }
}

int ks_strict_equal(KsValue a, KsValue b)
{
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case KS_UNDEF:
    case KS_NULL: return 1;
    case KS_BOOL: return (a.as.b != 0) == (b.as.b != 0);
    case KS_NUM: return a.as.num == b.as.num;
    case KS_STR: return ks_str_equal(KS_AS_STR(a), KS_AS_STR(b));
    default: return a.as.ref == b.as.ref;
    }
}

typedef struct Buf {
    char *p;
    size_t len, cap;
} Buf;

static void buf_put(Buf *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->len + n + 1) cap *= 2;
        b->p = xrealloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    b->p[b->len] = 0;
}

// Array.prototype.join(",") with JS cycle handling (a cyclic element joins as "").
static void tostring_into(KsValue v, Buf *b, const KsArray **seen, int depth)
{
    char num[32];
    switch (v.tag) {
    case KS_UNDEF: buf_put(b, "undefined", 9); return;
    case KS_NULL: buf_put(b, "null", 4); return;
    case KS_BOOL: v.as.b ? buf_put(b, "true", 4) : buf_put(b, "false", 5); return;
    case KS_NUM: buf_put(b, num, ks_num_format(v.as.num, num)); return;
    case KS_STR: buf_put(b, KS_AS_STR(v)->data, KS_AS_STR(v)->len); return;
    case KS_ARR: {
        const KsArray *a = KS_AS_ARR(v);
        for (int i = 0; i < depth; i++)
            if (seen[i] == a) return;
        if (depth >= 64) return;
        seen[depth] = a;
        for (uint32_t i = 0; i < a->len; i++) {
            if (i) buf_put(b, ",", 1);
            if (a->items[i].tag != KS_UNDEF && a->items[i].tag != KS_NULL) tostring_into(a->items[i], b, seen, depth + 1);
            if (b->len > (size_t)KS_MAX_STR_UNITS * 4) return;
        }
        return;
    }
    case KS_FN: buf_put(b, "[action]", 8); return;
    default: buf_put(b, "[object Object]", 15); return;
    }
}

KsStr *ks_to_str(KsScript *s, KsValue v)
{
    if (v.tag == KS_STR) return KS_AS_STR(v);
    if (v.tag == KS_NUM) {
        char num[32];
        size_t n = ks_num_format(v.as.num, num);
        return ks_str_new_raw(s, num, n, (uint32_t)n, 1);
    }
    Buf b = {0};
    const KsArray *seen[64];
    tostring_into(v, &b, seen, 0);
    KsStr *str = b.len ? ks_str_new(s, b.p, b.len) : s->s_empty;
    free(b.p);
    return str;
}

size_t ks_to_string(KsScript *s, KsValue v, char *out, size_t n)
{
    (void)s;
    Buf b = {0};
    const KsArray *seen[64];
    tostring_into(v, &b, seen, 0);
    size_t len = b.len;
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        if (c) memcpy(out, b.p, c);
        out[c] = 0;
    }
    free(b.p);
    return len;
}

double ks_to_number(KsValue v)
{
    switch (v.tag) {
    case KS_UNDEF: return NAN;
    case KS_NULL: return 0;
    case KS_BOOL: return v.as.b ? 1 : 0;
    case KS_NUM: return v.as.num;
    case KS_STR: return ks_string_to_number(KS_AS_STR(v)->data, KS_AS_STR(v)->len);
    case KS_ARR: {
        Buf b = {0};
        const KsArray *seen[64];
        tostring_into(v, &b, seen, 0);
        double d = ks_string_to_number(b.p ? b.p : "", b.len);
        free(b.p);
        return d;
    }
    default: return NAN;
    }
}

double ks_value_to_number(KsScript *s, KsValue v)
{
    (void)s;
    return ks_to_number(v);
}

// ---------------------------------------------------------------------------
// Public value constructors

KsValue ks_num(double d) { KsValue v; v.tag = KS_NUM; v.as.num = d; return v; }
KsValue ks_bool(int b) { KsValue v; v.tag = KS_BOOL; v.as.b = b != 0; return v; }
KsValue ks_undef(void) { KsValue v; v.tag = KS_UNDEF; v.as.num = 0; return v; }
KsValue ks_null(void) { KsValue v; v.tag = KS_NULL; v.as.num = 0; return v; }

KsValue ks_str(KsScript *s, const char *utf8, size_t len)
{
    return ks_str_value(ks_str_new(s, utf8 ? utf8 : "", utf8 ? len : 0));
}

KsValue ks_obj_new(KsScript *s) { return ks_ref_value(KS_OBJ, ks_object_new(s)); }
KsValue ks_arr_new(KsScript *s) { return ks_ref_value(KS_ARR, ks_array_new(s, 0)); }

void ks_obj_set(KsScript *s, KsValue obj, const char *key, KsValue v)
{
    if (obj.tag != KS_OBJ && obj.tag != KS_HOST) return;
    ks_set_prop(s, obj, ks_str_intern(s, key, strlen(key)), v);
}

KsValue ks_obj_get(KsScript *s, KsValue obj, const char *key)
{
    KsValue out = ks_undef();
    if (obj.tag != KS_OBJ && obj.tag != KS_HOST) return out;
    ks_get_prop(s, obj, ks_str_intern(s, key, strlen(key)), &out);
    return out;
}

size_t ks_obj_count(KsValue obj)
{
    return obj.tag == KS_OBJ ? KS_AS_OBJ(obj)->count : 0;
}

int ks_obj_entry(KsScript *s, KsValue obj, size_t i, KsValue *key, KsValue *value)
{
    (void)s;
    if (obj.tag != KS_OBJ || i >= KS_AS_OBJ(obj)->count) return 0;
    const KsProp *p = &KS_AS_OBJ(obj)->props[i];
    if (key) *key = ks_str_value(p->key);
    if (value) *value = p->val;
    return 1;
}

void ks_arr_push(KsScript *s, KsValue arr, KsValue v)
{
    if (arr.tag == KS_ARR) ks_array_push(s, KS_AS_ARR(arr), v);
}

size_t ks_arr_len(KsValue arr) { return arr.tag == KS_ARR ? KS_AS_ARR(arr)->len : 0; }

KsValue ks_arr_get(KsScript *s, KsValue arr, size_t i)
{
    (void)s;
    if (arr.tag != KS_ARR || i >= KS_AS_ARR(arr)->len) return ks_undef();
    return KS_AS_ARR(arr)->items[i];
}

const char *ks_str_utf8(KsValue v, size_t *len)
{
    if (v.tag != KS_STR) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = KS_AS_STR(v)->len;
    return KS_AS_STR(v)->data;
}

// ---------------------------------------------------------------------------
// Pins

void ks_handle_pin(KsScript *s, KsValue v)
{
    if (!ks_is_ref(v)) return;
    if (s->pin_count == s->pin_cap) {
        s->pin_cap = s->pin_cap ? s->pin_cap * 2 : 16;
        s->pins = xrealloc(s->pins, s->pin_cap * sizeof(KsValue));
    }
    s->pins[s->pin_count++] = v;
}

void ks_handle_unpin(KsScript *s, KsValue v)
{
    if (!ks_is_ref(v)) return;
    for (uint32_t i = s->pin_count; i-- > 0;) {
        if (s->pins[i].as.ref == v.as.ref) {
            s->pins[i] = s->pins[--s->pin_count];
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Garbage collector

static void gray_push(KsScript *s, KsObj *o)
{
    if (s->gray_len == s->gray_cap) {
        s->gray_cap = s->gray_cap ? s->gray_cap * 2 : 256;
        s->gray = xrealloc(s->gray, s->gray_cap * sizeof(KsObj *));
    }
    s->gray[s->gray_len++] = o;
}

static void mark_obj(KsScript *s, KsObj *o)
{
    if (!o || o->marked) return;
    o->marked = 1;
    if (o->type != KS_GC_STR) gray_push(s, o);
}

static void mark_value(KsScript *s, KsValue v)
{
    if (ks_is_ref(v)) mark_obj(s, v.as.ref);
}

static void trace(KsScript *s, KsObj *o)
{
    switch (o->type) {
    case KS_GC_ARR: {
        KsArray *a = (KsArray *)o;
        for (uint32_t i = 0; i < a->len; i++) mark_value(s, a->items[i]);
        break;
    }
    case KS_GC_OBJ: {
        KsObject *x = (KsObject *)o;
        for (uint32_t i = 0; i < x->count; i++) {
            mark_obj(s, &x->props[i].key->h);
            mark_value(s, x->props[i].val);
        }
        break;
    }
    case KS_GC_FN:
        mark_obj(s, (KsObj *)((KsFn *)o)->self);
        break;
    case KS_GC_HOST:
        mark_obj(s, (KsObj *)((KsHostObj *)o)->expando);
        break;
    default: break;
    }
}

void ks_gc_collect(KsScript *s)
{
    const KsProgram *p = s->prog;
    if (s->globals)
        for (uint32_t i = 0; i < p->global_count; i++) mark_value(s, s->globals[i]);
    for (uint32_t i = 0; i < s->sp; i++) mark_value(s, s->stack[i]);
    if (s->kstr)
        for (uint32_t i = 0; i < p->str_count; i++) mark_obj(s, (KsObj *)s->kstr[i]);
    if (s->action_fns)
        for (uint32_t i = 0; i < p->action_count; i++) mark_obj(s, (KsObj *)s->action_fns[i]);
    for (uint32_t i = 0; i < s->pin_count; i++) mark_value(s, s->pins[i]);
    for (uint32_t i = 0; i < s->hcache_cap; i++) mark_obj(s, (KsObj *)s->hcache[i].obj);
    for (uint32_t i = 0; i < s->class_key_count; i++)
        for (int k = 0; k < s->class_keys[i].cls->prop_count; k++) mark_obj(s, (KsObj *)s->class_keys[i].keys[k]);
    mark_obj(s, (KsObj *)s->s_empty);
    mark_obj(s, (KsObj *)s->s_key);
    mark_obj(s, (KsObj *)s->s_value);
    for (int i = 0; i < 128; i++) mark_obj(s, (KsObj *)s->chr[i]);
    while (s->gray_len) trace(s, s->gray[--s->gray_len]);

    // sweep
    KsObj **link = &s->objects;
    size_t live = 0;
    while (*link) {
        KsObj *o = *link;
        if (o->marked) {
            o->marked = 0;
            live += obj_size(o);
            link = &o->next;
        } else {
            *link = o->next;
            obj_free(o);
        }
    }
    s->bytes = live;
    s->gc_count++;

    // the intern table is weak: keep the survivors only
    if (s->intern_cap) {
        KsStr **old = s->intern;
        uint32_t old_cap = s->intern_cap;
        s->intern = xrealloc(NULL, old_cap * sizeof(KsStr *));
        memset(s->intern, 0, old_cap * sizeof(KsStr *));
        s->intern_count = 0;
        // Survivors are exactly the strings still on the object list.
        for (KsObj *o = s->objects; o; o = o->next)
            if (o->type == KS_GC_STR && ((KsStr *)o)->interned) intern_insert(s, (KsStr *)o);
        free(old);
    }

    size_t next = live * 2;
    if (next < KS_GC_MIN_THRESHOLD) next = KS_GC_MIN_THRESHOLD;
    if (next > KS_HEAP_LIMIT) next = KS_HEAP_LIMIT;
    s->gc_threshold = next;
}

void ks_gc_maybe(KsScript *s)
{
    if (s->bytes < s->gc_threshold || s->native_depth > 0) return;
    ks_gc_collect(s);
    if (s->bytes > KS_HEAP_LIMIT && !s->oom) {
        s->oom = 1;
        s->failed = 1;
        ks_raise(s, "Out of memory.");
    }
}

void ks_heap_free_all(KsScript *s)
{
    KsObj *o = s->objects;
    while (o) {
        KsObj *n = o->next;
        obj_free(o);
        o = n;
    }
    s->objects = NULL;
    s->bytes = 0;
    free(s->intern);
    s->intern = NULL;
    s->intern_cap = s->intern_count = 0;
    free(s->gray);
    s->gray = NULL;
    s->gray_len = s->gray_cap = 0;
    free(s->pins);
    s->pins = NULL;
    s->pin_count = s->pin_cap = 0;
    free(s->hcache);
    s->hcache = NULL;
    s->hcache_count = s->hcache_cap = 0;
    for (uint32_t i = 0; i < s->class_key_count; i++) free(s->class_keys[i].keys);
    free(s->class_keys);
    s->class_keys = NULL;
    s->class_key_count = 0;
}

// Test hook (shared/tests/ks_vm_test.c): bytes currently accounted to the heap.
size_t ks_test_heap_bytes(KsScript *s) { return s->bytes; }

// F(x) = parseFloat(x) || 0: the host-setter / (num) coercion (spec 1.13, 3.2).
double ks_to_float(KsValue v)
{
    double d;
    if (v.tag == KS_NUM) d = v.as.num;
    else if (v.tag == KS_STR) d = ks_parse_float(KS_AS_STR(v)->data, KS_AS_STR(v)->len);
    else if (v.tag == KS_ARR) {
        Buf b = {0};
        const KsArray *seen[64];
        tostring_into(v, &b, seen, 0);
        d = ks_parse_float(b.p ? b.p : "", b.len);
        free(b.p);
    } else d = NAN; // undefined, null, booleans, objects, actions: parseFloat(String(x)) is NaN
    return (isnan(d) || d == 0) ? 0 : d;
}
