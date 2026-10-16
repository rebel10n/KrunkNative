// Official Krunker socket codec (shared/include/krunker_proto.h).
//
// The msgpack reader/writer below is a minimal mirror of services/common/src/msgpack.c (the
// knsvc pull parser and put_be writer), kept here instead of moving that file because shared/
// must not link services/ (kn_buf, cJSON bridge) and because the number rule differs:
// knsvc follows msgpack-lite (int32 range -> int, else float64), the official client uses
// @msgpack/msgpack (every safe integer -> smallest int, uint64 / int64 included).
#include "krunker_proto.h"
#include "krunker_build_live.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- packet name tables

#if KRUNKER_BUILD_SCHEMA != 1
#error "krunker_build.h schema changed: update krunker_proto.c"
#endif

// Ids are krunker_build.h's (bundle keys); the wire names are the current build's
// (krunker_build_live.h: the compiled header's, or a published build.json's), NULL when it has none.
#define KRUNKER_X_NAME_(ident, wire, name) name,
static const char *const C2S_KEY[KRUNKER_C2S_COUNT] = {KRUNKER_C2S_PACKETS(KRUNKER_X_NAME_)};
static const char *const S2C_KEY[KRUNKER_S2C_COUNT] = {KRUNKER_S2C_PACKETS(KRUNKER_X_NAME_)};
#undef KRUNKER_X_NAME_

const char *krunker_c2s_wire(KrunkerC2S id) { return (unsigned)id < KRUNKER_C2S_COUNT ? krunker_build()->c2s_wire[id] : NULL; }
const char *krunker_c2s_key(KrunkerC2S id) { return (unsigned)id < KRUNKER_C2S_COUNT ? C2S_KEY[id] : NULL; }
const char *krunker_s2c_wire(KrunkerS2C id) { return (unsigned)id < KRUNKER_S2C_COUNT ? krunker_build()->s2c_wire[id] : NULL; }
const char *krunker_s2c_key(KrunkerS2C id) { return (unsigned)id < KRUNKER_S2C_COUNT ? S2C_KEY[id] : NULL; }

static int lookup(const char *const *table, int count, const char *wire, size_t len) {
    if (!wire) return -1;
    for (int i = 0; i < count; i++)
        if (table[i] && strlen(table[i]) == len && memcmp(table[i], wire, len) == 0) return i;
    return -1;
}

int krunker_c2s_lookup(const char *wire, size_t len) { return lookup(krunker_build()->c2s_wire, KRUNKER_C2S_COUNT, wire, len); }
int krunker_s2c_lookup(const char *wire, size_t len) { return lookup(krunker_build()->s2c_wire, KRUNKER_S2C_COUNT, wire, len); }

// ---------------------------------------------------------------- values

int krunker_is(const KrunkerValue *v, KrunkerValueType t) { return v && v->type == (uint8_t)t; }

const KrunkerValue *krunker_at(const KrunkerValue *array, uint32_t i) {
    return krunker_is(array, KRUNKER_ARRAY) && i < array->len ? &array->u.items[i] : NULL;
}

// JS String(n) of a safe integer (Object keys); 0 when n is not one.
static int int_key_text(double n, char *out, size_t cap) {
    if (!isfinite(n) || n != floor(n) || fabs(n) > 9007199254740991.0) return 0;
    snprintf(out, cap, "%lld", (long long)n);
    return 1;
}

const KrunkerValue *krunker_get(const KrunkerValue *map, const char *key) {
    if (!krunker_is(map, KRUNKER_MAP) || !key) return NULL;
    const size_t klen = strlen(key);
    const KrunkerValue *found = NULL;
    for (uint32_t i = 0; i < map->len; i++) {
        const KrunkerValue *k = &map->u.items[2 * (size_t)i];
        char num[32];
        if (k->type == KRUNKER_STR) {
            if (k->len == klen && memcmp(k->u.str, key, klen) == 0) found = k + 1;
        } else if (k->type == KRUNKER_NUM && int_key_text(k->u.num, num, sizeof num)) {
            if (strcmp(num, key) == 0) found = k + 1;
        }
    }
    return found; // duplicate keys: the last one wins, as in JS object assignment
}

double krunker_num(const KrunkerValue *v, double def) {
    if (krunker_is(v, KRUNKER_NUM)) return v->u.num;
    if (krunker_is(v, KRUNKER_BOOL)) return v->u.boolean ? 1.0 : 0.0;
    return def;
}

const char *krunker_str(const KrunkerValue *v) { return krunker_is(v, KRUNKER_STR) ? v->u.str : NULL; }

// ---------------------------------------------------------------- byte buffer

void krunker_buf_free(KrunkerBuf *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int buf_put(KrunkerBuf *b, const void *p, size_t n) {
    if (n > SIZE_MAX - b->len) return -1;
    const size_t need = b->len + n;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < need) cap = cap > SIZE_MAX / 2 ? need : cap * 2;
        uint8_t *d = realloc(b->data, cap);
        if (!d) return -1;
        b->data = d;
        b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, p, n);
    b->len = need;
    return 0;
}

static int buf_byte(KrunkerBuf *b, uint8_t c) { return buf_put(b, &c, 1); }

static int buf_text(KrunkerBuf *b, const char *s) { return buf_put(b, s, strlen(s)); }

// ---------------------------------------------------------------- msgpack writer (@msgpack/msgpack Encoder)

static int put_be(KrunkerBuf *b, uint8_t tag, uint64_t v, int bytes) {
    uint8_t tmp[9];
    tmp[0] = tag;
    for (int i = 0; i < bytes; i++) tmp[1 + i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
    return buf_put(b, tmp, (size_t)bytes + 1);
}

// Encoder.encodeNumber: Number.isSafeInteger -> smallest int format, else float64.
static int w_number(KrunkerBuf *b, double d) {
    if (isfinite(d) && d == floor(d) && fabs(d) <= 9007199254740991.0) {
        if (d >= 0) { // -0 included, like JS (-0 >= 0)
            const uint64_t u = (uint64_t)d;
            if (u < 0x80) return buf_byte(b, (uint8_t)u);
            if (u < 0x100) return put_be(b, 0xcc, u, 1);
            if (u < 0x10000) return put_be(b, 0xcd, u, 2);
            if (u < 0x100000000ull) return put_be(b, 0xce, u, 4);
            return put_be(b, 0xcf, u, 8);
        }
        const int64_t i = (int64_t)d;
        if (i >= -0x20) return buf_byte(b, (uint8_t)(0xe0 | (i + 0x20)));
        if (i >= -0x80) return put_be(b, 0xd0, (uint64_t)i & 0xff, 1);
        if (i >= -0x8000) return put_be(b, 0xd1, (uint64_t)i & 0xffff, 2);
        if (i >= -0x80000000ll) return put_be(b, 0xd2, (uint64_t)i & 0xffffffffu, 4);
        return put_be(b, 0xd3, (uint64_t)i, 8);
    }
    uint64_t bits;
    memcpy(&bits, &d, 8);
    return put_be(b, 0xcb, bits, 8);
}

static int w_str(KrunkerBuf *b, const char *s, uint32_t len) {
    int r;
    if (len < 0x20) r = buf_byte(b, (uint8_t)(0xa0 + len));
    else if (len < 0x100) r = put_be(b, 0xd9, len, 1);
    else if (len < 0x10000) r = put_be(b, 0xda, len, 2);
    else r = put_be(b, 0xdb, len, 4);
    return r ? r : buf_put(b, s, len);
}

static int w_count(KrunkerBuf *b, uint32_t n, uint8_t fix, uint8_t tag16, uint8_t tag32) {
    if (n < 0x10) return buf_byte(b, (uint8_t)(fix + n));
    if (n < 0x10000) return put_be(b, tag16, n, 2);
    return put_be(b, tag32, n, 4);
}

static int w_value(KrunkerBuf *b, const KrunkerValue *v, int depth) {
    if (depth > KRUNKER_PROTO_MAX_DEPTH) return KRUNKER_PROTO_EDEPTH;
    if (!v) return KRUNKER_PROTO_EINVAL;
    if ((v->type == KRUNKER_STR || v->type == KRUNKER_BIN || v->type == KRUNKER_EXT || v->type == KRUNKER_ARRAY ||
         v->type == KRUNKER_MAP) && v->len && !v->u.str)
        return KRUNKER_PROTO_EINVAL; // u.str / u.items share storage: a NULL pointer with items
    int r;
    switch ((KrunkerValueType)v->type) {
    case KRUNKER_NIL: r = buf_byte(b, 0xc0); break;
    case KRUNKER_BOOL: r = buf_byte(b, v->u.boolean ? 0xc3 : 0xc2); break;
    case KRUNKER_NUM: r = w_number(b, v->u.num); break;
    case KRUNKER_STR: r = w_str(b, v->u.str, v->len); break;
    case KRUNKER_BIN:
        if (v->len < 0x100) r = put_be(b, 0xc4, v->len, 1);
        else if (v->len < 0x10000) r = put_be(b, 0xc5, v->len, 2);
        else r = put_be(b, 0xc6, v->len, 4);
        if (!r) r = buf_put(b, v->u.str, v->len);
        break;
    case KRUNKER_EXT: {
        static const uint8_t fixext[17] = {[1] = 0xd4, [2] = 0xd5, [4] = 0xd6, [8] = 0xd7, [16] = 0xd8};
        if (v->len <= 16 && fixext[v->len]) r = buf_byte(b, fixext[v->len]);
        else if (v->len < 0x100) r = put_be(b, 0xc7, v->len, 1);
        else if (v->len < 0x10000) r = put_be(b, 0xc8, v->len, 2);
        else r = put_be(b, 0xc9, v->len, 4);
        if (!r) r = buf_byte(b, (uint8_t)v->ext_type);
        if (!r) r = buf_put(b, v->u.str, v->len);
        break;
    }
    case KRUNKER_ARRAY:
        if (w_count(b, v->len, 0x90, 0xdc, 0xdd)) return KRUNKER_PROTO_ENOMEM;
        for (uint32_t i = 0; i < v->len; i++)
            if ((r = w_value(b, &v->u.items[i], depth + 1))) return r;
        return KRUNKER_PROTO_OK;
    case KRUNKER_MAP:
        if (w_count(b, v->len, 0x80, 0xde, 0xdf)) return KRUNKER_PROTO_ENOMEM;
        for (uint32_t i = 0; i < v->len; i++) {
            const KrunkerValue *k = &v->u.items[2 * (size_t)i];
            char num[32];
            if (k->type == KRUNKER_STR && (k->u.str || !k->len)) r = w_str(b, k->u.str, k->len);
            else if (k->type == KRUNKER_NUM && int_key_text(k->u.num, num, sizeof num)) r = w_str(b, num, (uint32_t)strlen(num));
            else return KRUNKER_PROTO_EINVAL;
            if (r) return KRUNKER_PROTO_ENOMEM;
            if ((r = w_value(b, k + 1, depth + 1))) return r;
        }
        return KRUNKER_PROTO_OK;
    default: return KRUNKER_PROTO_EINVAL;
    }
    return r ? KRUNKER_PROTO_ENOMEM : KRUNKER_PROTO_OK;
}

// ---------------------------------------------------------------- session

void krunker_session_init(KrunkerSession *s, int64_t ahkey) {
    s->ahkey = ahkey;
    s->ah_num = 0;
}

void krunker_session_reset(KrunkerSession *s) { s->ah_num = 0; }

int krunker_proto_ahnum_exempt(const char *name, size_t name_len) {
    const char *exempt = krunker_build()->ahnum_exempt_wire; // the current build's (cptR)
    return name && name_len == strlen(exempt) && memcmp(name, exempt, name_len) == 0;
}

// JS (a + AHKEY) & 0xff on int32: only the low 8 bits of the sum survive, so it is the sum
// mod 256 for any integral AHKEY (negative ones too, two's complement).
static uint8_t ah_step(uint8_t ah, int64_t key) { return (uint8_t)(((uint64_t)ah + (uint64_t)key) & 0xff); }

uint8_t krunker_proto_next_ahnum(KrunkerSession *s, const char *name) {
    if (!krunker_proto_ahnum_exempt(name, name ? strlen(name) : 0)) s->ah_num = ah_step(s->ah_num, s->ahkey);
    return s->ah_num;
}

// ---------------------------------------------------------------- frames: encode

int krunker_proto_encode_ah(uint8_t ah_num, const char *name, const KrunkerValue *args, size_t argc,
                            KrunkerBuf *out) {
    if (!name || !out || (argc && !args) || argc >= 0xffffffffu) return KRUNKER_PROTO_EINVAL;
    const size_t name_len = strlen(name);
    if (name_len > 0xffffffffu) return KRUNKER_PROTO_EINVAL;
    out->len = 0;
    // [name].concat(args) is the outer array (depth 1), its items are depth 2
    int r = w_count(out, (uint32_t)argc + 1, 0x90, 0xdc, 0xdd) || w_str(out, name, (uint32_t)name_len)
                ? KRUNKER_PROTO_ENOMEM : KRUNKER_PROTO_OK;
    for (size_t i = 0; !r && i < argc; i++) r = w_value(out, &args[i], 2);
    const uint8_t trailer[2] = {(uint8_t)(ah_num >> 4 & 0xf), (uint8_t)(ah_num & 0xf)};
    if (!r && buf_put(out, trailer, 2)) r = KRUNKER_PROTO_ENOMEM;
    if (r) out->len = 0;
    return r;
}

int krunker_proto_encode(KrunkerSession *s, const char *name, const KrunkerValue *args, size_t argc,
                         KrunkerBuf *out) {
    if (!s || !name) return KRUNKER_PROTO_EINVAL;
    KrunkerSession next = *s;
    const uint8_t ah = krunker_proto_next_ahnum(&next, name);
    const int r = krunker_proto_encode_ah(ah, name, args, argc, out);
    if (r == KRUNKER_PROTO_OK) *s = next;
    return r;
}

// ---------------------------------------------------------------- msgpack reader (@msgpack/msgpack Decoder)

typedef struct Reader {
    const uint8_t *p;
    size_t len;
    size_t pos;
} Reader;

typedef struct Head {
    KrunkerValueType type;
    int8_t ext_type;
    uint32_t len;        // STR/BIN/EXT bytes, ARRAY items, MAP pairs
    double num;
    int boolean;
    const uint8_t *data; // STR/BIN/EXT bytes (inside the frame)
} Head;

static int need(const Reader *r, size_t n) { return r->len - r->pos >= n; }

static uint64_t get_be(Reader *r, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | r->p[r->pos + (size_t)i];
    r->pos += (size_t)bytes;
    return v;
}

static int read_blob(Reader *r, Head *h, KrunkerValueType t, uint64_t len) {
    if (!need(r, len)) return KRUNKER_PROTO_EMSGPACK;
    h->type = t;
    h->len = (uint32_t)len;
    h->data = r->p + r->pos;
    r->pos += len;
    return 0;
}

static int read_ext(Reader *r, Head *h, uint64_t len) {
    if (!need(r, 1)) return KRUNKER_PROTO_EMSGPACK;
    h->ext_type = (int8_t)r->p[r->pos++];
    return read_blob(r, h, KRUNKER_EXT, len);
}

// One value head; the bytes of STR/BIN/EXT are consumed (h->data), children of ARRAY/MAP are not.
static int read_head(Reader *r, Head *h) {
    memset(h, 0, sizeof *h);
    if (!need(r, 1)) return KRUNKER_PROTO_EMSGPACK;
    const uint8_t c = r->p[r->pos++];
    if (c <= 0x7f) { h->type = KRUNKER_NUM; h->num = c; return 0; }
    if (c >= 0xe0) { h->type = KRUNKER_NUM; h->num = (int8_t)c; return 0; }
    if (c <= 0x8f) { h->type = KRUNKER_MAP; h->len = c & 15u; return 0; }
    if (c <= 0x9f) { h->type = KRUNKER_ARRAY; h->len = c & 15u; return 0; }
    if (c <= 0xbf) return read_blob(r, h, KRUNKER_STR, c & 31u);
    int bytes;
    switch (c) {
    case 0xc0: h->type = KRUNKER_NIL; return 0;
    case 0xc2: case 0xc3: h->type = KRUNKER_BOOL; h->boolean = c == 0xc3; return 0;
    case 0xc4: case 0xc5: case 0xc6:
        bytes = 1 << (c - 0xc4);
        return need(r, (size_t)bytes) ? read_blob(r, h, KRUNKER_BIN, get_be(r, bytes)) : KRUNKER_PROTO_EMSGPACK;
    case 0xc7: case 0xc8: case 0xc9:
        bytes = 1 << (c - 0xc7);
        return need(r, (size_t)bytes) ? read_ext(r, h, get_be(r, bytes)) : KRUNKER_PROTO_EMSGPACK;
    case 0xca: case 0xcb: {
        bytes = c == 0xca ? 4 : 8;
        if (!need(r, (size_t)bytes)) return KRUNKER_PROTO_EMSGPACK;
        const uint64_t u = get_be(r, bytes);
        h->type = KRUNKER_NUM;
        if (bytes == 4) {
            const uint32_t u32 = (uint32_t)u;
            float f;
            memcpy(&f, &u32, 4);
            h->num = f;
        } else {
            memcpy(&h->num, &u, 8);
        }
        return 0;
    }
    case 0xcc: case 0xcd: case 0xce: case 0xcf:
        bytes = 1 << (c - 0xcc);
        if (!need(r, (size_t)bytes)) return KRUNKER_PROTO_EMSGPACK;
        h->type = KRUNKER_NUM;
        h->num = (double)get_be(r, bytes); // JS: high * 2^32 + low, the same rounding
        return 0;
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        bytes = 1 << (c - 0xd0);
        if (!need(r, (size_t)bytes)) return KRUNKER_PROTO_EMSGPACK;
        const uint64_t u = get_be(r, bytes);
        const int shift = 64 - 8 * bytes;
        // sign-extend through the unsigned type (no UB on the left shift)
        const uint64_t ext = shift ? (u & (1ull << (8 * bytes - 1)) ? u | ~0ull << (8 * bytes) : u) : u;
        int64_t i;
        memcpy(&i, &ext, 8);
        h->type = KRUNKER_NUM;
        h->num = (double)i;
        return 0;
    }
    case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8: return read_ext(r, h, 1u << (c - 0xd4));
    case 0xd9: case 0xda: case 0xdb:
        bytes = 1 << (c - 0xd9);
        return need(r, (size_t)bytes) ? read_blob(r, h, KRUNKER_STR, get_be(r, bytes)) : KRUNKER_PROTO_EMSGPACK;
    case 0xdc: case 0xdd: case 0xde: case 0xdf:
        bytes = c == 0xdc || c == 0xde ? 2 : 4;
        if (!need(r, (size_t)bytes)) return KRUNKER_PROTO_EMSGPACK;
        h->type = c <= 0xdd ? KRUNKER_ARRAY : KRUNKER_MAP;
        h->len = (uint32_t)get_be(r, bytes);
        return 0;
    default: return KRUNKER_PROTO_EMSGPACK; // 0xc1: never used
    }
}

static int is_proto_key(const Head *k) { return k->type == KRUNKER_STR && k->len == 9 && memcmp(k->data, "__proto__", 9) == 0; }

// Pass 1: validate one value and count its nodes and string bytes (+ a NUL each).
static int count_value(Reader *r, int depth, size_t *nodes, size_t *bytes) {
    if (depth > KRUNKER_PROTO_MAX_DEPTH) return KRUNKER_PROTO_EDEPTH;
    Head h;
    int e = read_head(r, &h);
    if (e) return e;
    ++*nodes;
    if (h.type == KRUNKER_STR || h.type == KRUNKER_BIN || h.type == KRUNKER_EXT) *bytes += (size_t)h.len + 1;
    if (h.type == KRUNKER_ARRAY) {
        if (h.len > r->len - r->pos) return KRUNKER_PROTO_EMSGPACK; // every item needs >= 1 byte
        for (uint32_t i = 0; i < h.len; i++)
            if ((e = count_value(r, depth + 1, nodes, bytes))) return e;
    } else if (h.type == KRUNKER_MAP) {
        if ((uint64_t)h.len * 2 > r->len - r->pos) return KRUNKER_PROTO_EMSGPACK;
        for (uint32_t i = 0; i < h.len; i++) {
            // the Decoder: "The type of key must be string or number" / "__proto__ is not allowed"
            const size_t at = r->pos;
            Head k;
            if ((e = read_head(r, &k))) return e;
            if ((k.type != KRUNKER_STR && k.type != KRUNKER_NUM) || is_proto_key(&k)) return KRUNKER_PROTO_EKEY;
            r->pos = at;
            if ((e = count_value(r, depth + 1, nodes, bytes)) || (e = count_value(r, depth + 1, nodes, bytes))) return e;
        }
    }
    return 0;
}

typedef struct Fill {
    KrunkerValue *next;  // next free node
    char *text;          // next free string byte
} Fill;

// Pass 2 over the validated bytes: children of an array / map take consecutive nodes.
static void fill_value(Reader *r, KrunkerValue *slot, Fill *f) {
    Head h;
    read_head(r, &h); // validated by count_value
    slot->type = (uint8_t)h.type;
    slot->ext_type = h.ext_type;
    slot->len = h.len;
    switch (h.type) {
    case KRUNKER_BOOL: slot->u.boolean = h.boolean; break;
    case KRUNKER_NUM: slot->u.num = h.num; break;
    case KRUNKER_STR: case KRUNKER_BIN: case KRUNKER_EXT:
        if (h.len) memcpy(f->text, h.data, h.len);
        f->text[h.len] = 0;
        slot->u.str = f->text;
        f->text += (size_t)h.len + 1;
        break;
    case KRUNKER_ARRAY: case KRUNKER_MAP: {
        const size_t n = h.type == KRUNKER_MAP ? 2 * (size_t)h.len : h.len;
        KrunkerValue *items = f->next;
        f->next += n;
        slot->u.items = items;
        for (size_t i = 0; i < n; i++) fill_value(r, &items[i], f);
        break;
    }
    default: break;
    }
}

int krunker_proto_decode(const void *data, size_t len, unsigned flags, KrunkerFrame *out) {
    if (!out) return KRUNKER_PROTO_EINVAL;
    memset(out, 0, sizeof *out);
    if (!data) return KRUNKER_PROTO_EINVAL;
    if (len < 3) return KRUNKER_PROTO_ESHORT;
    const uint8_t *p = data;
    const uint8_t t0 = p[len - 2], t1 = p[len - 1];
    if ((flags & KRUNKER_DECODE_STRICT_TRAILER) && (t0 > 0xf || t1 > 0xf)) return KRUNKER_PROTO_ETRAILER;

    Reader r = {p, len - 2, 0};
    size_t nodes = 0, bytes = 0;
    int e = count_value(&r, 1, &nodes, &bytes);
    if (e) return e;
    if (r.pos != r.len) return KRUNKER_PROTO_EEXTRA;

    r.pos = 0;
    Head root;
    read_head(&r, &root);
    if (root.type != KRUNKER_ARRAY || root.len < 1) return KRUNKER_PROTO_ENOTARRAY;

    if (nodes > (SIZE_MAX - bytes) / sizeof(KrunkerValue)) return KRUNKER_PROTO_ENOMEM;
    KrunkerValue *mem = malloc(nodes * sizeof(KrunkerValue) + bytes);
    if (!mem) return KRUNKER_PROTO_ENOMEM;
    Fill f = {mem + 1, (char *)(mem + nodes)};
    r.pos = 0;
    fill_value(&r, mem, &f);

    const KrunkerValue *name = &mem->u.items[0];
    if (name->type != KRUNKER_STR) {
        free(mem);
        return KRUNKER_PROTO_ENAME;
    }
    out->ah_echo = (t0 << 4) + t1;
    out->name = name->u.str;
    out->name_len = name->len;
    out->args = mem->u.items + 1;
    out->argc = mem->len - 1;
    out->mem_ = mem;
    return KRUNKER_PROTO_OK;
}

void krunker_frame_free(KrunkerFrame *f) {
    if (!f) return;
    free(f->mem_);
    memset(f, 0, sizeof *f);
}

const KrunkerValue *krunker_frame_arg(const KrunkerFrame *f, uint32_t i) {
    return f && f->args && i < f->argc ? &f->args[i] : NULL;
}

const char *krunker_proto_strerror(int err) {
    switch ((KrunkerProtoError)err) {
    case KRUNKER_PROTO_OK: return "ok";
    case KRUNKER_PROTO_EINVAL: return "invalid argument";
    case KRUNKER_PROTO_ENOMEM: return "out of memory";
    case KRUNKER_PROTO_ESHORT: return "frame too short";
    case KRUNKER_PROTO_EMSGPACK: return "truncated or invalid msgpack";
    case KRUNKER_PROTO_EEXTRA: return "extra bytes after the msgpack value";
    case KRUNKER_PROTO_ENOTARRAY: return "payload is not a non-empty array";
    case KRUNKER_PROTO_ENAME: return "packet name is not a string";
    case KRUNKER_PROTO_EDEPTH: return "nesting too deep";
    case KRUNKER_PROTO_EKEY: return "map key is not a string or number";
    case KRUNKER_PROTO_ETRAILER: return "invalid ahNum trailer";
    }
    return "unknown error";
}

// ---------------------------------------------------------------- JSON dump (logs)

static int json_hex(KrunkerBuf *b, const char *p, uint32_t n) {
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        const char two[2] = {hex[(uint8_t)p[i] >> 4], hex[(uint8_t)p[i] & 15]};
        if (buf_put(b, two, 2)) return -1;
    }
    return 0;
}

static int json_str(KrunkerBuf *b, const char *s, uint32_t n) {
    if (buf_byte(b, '"')) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = (uint8_t)s[i];
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            if (buf_put(b, esc, 2)) return -1;
        } else if (c < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", c);
            if (buf_put(b, esc, 6)) return -1;
        } else if (buf_byte(b, c)) {
            return -1;
        }
    }
    return buf_byte(b, '"');
}

static int json_num(KrunkerBuf *b, double d) {
    char tmp[40];
    if (!isfinite(d)) return buf_text(b, "null");
    if (!int_key_text(d, tmp, sizeof tmp)) snprintf(tmp, sizeof tmp, "%.17g", d);
    return buf_text(b, tmp);
}

static int json_value(KrunkerBuf *b, const KrunkerValue *v, int depth) {
    if (depth > KRUNKER_PROTO_MAX_DEPTH) return KRUNKER_PROTO_EDEPTH;
    int r = 0;
    switch ((KrunkerValueType)v->type) {
    case KRUNKER_NIL: r = buf_text(b, "null"); break;
    case KRUNKER_BOOL: r = buf_text(b, v->u.boolean ? "true" : "false"); break;
    case KRUNKER_NUM: r = json_num(b, v->u.num); break;
    case KRUNKER_STR: r = json_str(b, v->u.str, v->len); break;
    case KRUNKER_BIN: r = buf_text(b, "{\"bin\":\"") || json_hex(b, v->u.str, v->len) || buf_text(b, "\"}"); break;
    case KRUNKER_EXT: {
        char head[32];
        snprintf(head, sizeof head, "{\"ext\":%d,\"hex\":\"", v->ext_type);
        r = buf_text(b, head) || json_hex(b, v->u.str, v->len) || buf_text(b, "\"}");
        break;
    }
    case KRUNKER_ARRAY:
        if (buf_byte(b, '[')) return KRUNKER_PROTO_ENOMEM;
        for (uint32_t i = 0; i < v->len; i++) {
            if (i && buf_byte(b, ',')) return KRUNKER_PROTO_ENOMEM;
            if ((r = json_value(b, &v->u.items[i], depth + 1))) return r;
        }
        r = buf_byte(b, ']');
        break;
    case KRUNKER_MAP:
        if (buf_byte(b, '{')) return KRUNKER_PROTO_ENOMEM;
        for (uint32_t i = 0; i < v->len; i++) {
            const KrunkerValue *k = &v->u.items[2 * (size_t)i];
            if (i && buf_byte(b, ',')) return KRUNKER_PROTO_ENOMEM;
            if (k->type == KRUNKER_STR ? json_str(b, k->u.str, k->len)
                                       : buf_byte(b, '"') || json_num(b, krunker_num(k, 0)) || buf_byte(b, '"'))
                return KRUNKER_PROTO_ENOMEM;
            if (buf_byte(b, ':')) return KRUNKER_PROTO_ENOMEM;
            if ((r = json_value(b, k + 1, depth + 1))) return r;
        }
        r = buf_byte(b, '}');
        break;
    default: r = buf_text(b, "null"); break;
    }
    return r ? KRUNKER_PROTO_ENOMEM : KRUNKER_PROTO_OK;
}

int krunker_value_to_json(const KrunkerValue *v, KrunkerBuf *out) {
    if (!v || !out) return KRUNKER_PROTO_EINVAL;
    return json_value(out, v, 1);
}
