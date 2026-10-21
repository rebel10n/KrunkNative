/*
 * v10_translate.c - the Krunker v10 <-> KrunkerBox native packet mappings (see v10_translate.h).
 *
 * Each function is the C form of a function of net-bridge/v10Translate.ts, with the same field
 * order, the same arguments and the same stop reasons. The byte offsets are the NetXxxPacket
 * layouts that the TS comments name (shared/include/shared.h); the tests replay the TS fixtures.
 *
 * JavaScript semantics the TS relies on are kept where a value can carry them: Number(x) of an
 * unknown value (js_num), truthiness (js_truthy), `x || 1` (js_or1), Math.round (js_round),
 * ToInt32 for `n | 0` (js_int32), and the sanitised UTF-8 of Buffer.toString("utf8") (text fields
 * become UTF-8 with each invalid byte replaced by U+FFFD; Node replaces maximal subparts, so a
 * damaged multi-byte sequence can differ by a replacement character).
 */
#include "v10_translate.h"

#include "shared.h" /* NET_PROTOCOL_MAJOR / MINOR / PATCH, NET_HELLO_RULESETS: the HELLO this bridge sends */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V10_MAX_CHUNKS_TOTAL ((size_t)128 * 1024 * 1024) /* a map or config text this large is refused */

/* ---- native packet field readers (little endian; the callers check the lengths) */

static int32_t rd_i32(const uint8_t *p, size_t at) {
    return (int32_t)((uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24);
}
static uint32_t rd_u32(const uint8_t *p, size_t at) {
    return (uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24;
}
static uint16_t rd_u16(const uint8_t *p, size_t at) { return (uint16_t)(p[at] | p[at + 1] << 8); }
static int16_t rd_i16(const uint8_t *p, size_t at) { return (int16_t)rd_u16(p, at); }
static double rd_f32(const uint8_t *p, size_t at) {
    uint32_t u = rd_u32(p, at);
    float f;
    memcpy(&f, &u, sizeof f);
    return (double)f;
}
static void wr_i32(uint8_t *p, size_t at, int64_t v) {
    const uint32_t u = (uint32_t)v; /* the low 32 bits: Buffer.writeInt32LE for an int32 value */
    p[at] = (uint8_t)u;
    p[at + 1] = (uint8_t)(u >> 8);
    p[at + 2] = (uint8_t)(u >> 16);
    p[at + 3] = (uint8_t)(u >> 24);
}
static void wr_f32(uint8_t *p, size_t at, double v) {
    const float f = (float)v;
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    wr_i32(p, at, (int64_t)u);
}

/* The length of the NUL-terminated field of `size` bytes at `at` (TS: str / cText): the bytes up to
 * the first NUL inside the field, or the whole field. Bounded by the packet's length n. */
static size_t field_len(const uint8_t *p, size_t n, size_t at, size_t size) {
    size_t end = at + size;
    if (end > n) end = n;
    size_t i = at;
    while (i < end && p[i]) i++;
    return i - at;
}

/* ---- UTF-8 (Buffer.toString("utf8") replaces each invalid byte with U+FFFD) */

static int is_cont(uint8_t b) { return (b & 0xC0) == 0x80; }

/* The length of the valid UTF-8 sequence at s (n bytes available), 0 when there is none at s. */
static size_t utf8_seq(const uint8_t *s, size_t n) {
    const uint8_t c = s[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return n >= 2 && is_cont(s[1]) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || !is_cont(s[1]) || !is_cont(s[2])) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0;
        if (c == 0xED && s[1] > 0x9F) return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || !is_cont(s[1]) || !is_cont(s[2]) || !is_cont(s[3])) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0;
        if (c == 0xF4 && s[1] > 0x8F) return 0;
        return 4;
    }
    return 0;
}

/* Copies s[0..n) into dst as valid UTF-8 (dst needs 3 * n bytes); returns the length. */
static size_t utf8_sanitize(const uint8_t *s, size_t n, uint8_t *dst) {
    size_t i = 0, o = 0;
    while (i < n) {
        const size_t len = utf8_seq(s + i, n - i);
        if (len == 0) {
            dst[o++] = 0xEF; /* U+FFFD */
            dst[o++] = 0xBF;
            dst[o++] = 0xBD;
            i++;
            continue;
        }
        memcpy(dst + o, s + i, len);
        o += len;
        i += len;
    }
    return o;
}

/* The code point at s (valid UTF-8 after utf8_sanitize, avail bytes left) and its length. */
static uint32_t cp_at(const uint8_t *s, size_t avail, size_t *len) {
    const size_t l = utf8_seq(s, avail);
    *len = l ? l : 1;
    if (l == 0) return 0xFFFD;
    if (l == 1) return s[0];
    if (l == 2) return ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    if (l == 3) return ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
}

/* JavaScript's String.prototype.trim whitespace (WhiteSpace and LineTerminator). */
static int js_is_space(uint32_t cp) {
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return 1;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

/* ---- JavaScript value semantics over the decoded KrunkerValue (NULL is undefined) */

static int ascii_space(uint8_t c) { return c == ' ' || (c >= 0x09 && c <= 0x0D); }

/* Number(string): the whole string is a decimal or hex literal, Infinity, or blank (0); else NaN. */
static double js_str_to_num(const uint8_t *s, size_t n) {
    size_t a = 0, b = n;
    while (a < b && ascii_space(s[a])) a++;
    while (b > a && ascii_space(s[b - 1])) b--;
    if (a == b) return 0;
    char buf[128];
    if (b - a >= sizeof buf) return NAN;
    memcpy(buf, s + a, b - a);
    buf[b - a] = 0;
    if (!strcmp(buf, "Infinity") || !strcmp(buf, "+Infinity")) return INFINITY;
    if (!strcmp(buf, "-Infinity")) return -INFINITY;
    if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        if (buf[2] == 0) return NAN;
        double v = 0;
        for (const char *h = buf + 2; *h; h++) {
            if (!isxdigit((unsigned char)*h)) return NAN;
            v = v * 16 + (isdigit((unsigned char)*h) ? *h - '0' : (tolower((unsigned char)*h) - 'a' + 10));
        }
        return v;
    }
    /* decimal literal: [+-] digits [. digits] [e [+-] digits], at least one digit in the mantissa */
    const char *q = buf;
    if (*q == '+' || *q == '-') q++;
    int digits = 0;
    while (isdigit((unsigned char)*q)) q++, digits++;
    if (*q == '.') {
        q++;
        while (isdigit((unsigned char)*q)) q++, digits++;
    }
    if (digits == 0) return NAN;
    if (*q == 'e' || *q == 'E') {
        q++;
        if (*q == '+' || *q == '-') q++;
        int ed = 0;
        while (isdigit((unsigned char)*q)) q++, ed++;
        if (ed == 0) return NAN;
    }
    if (*q) return NAN;
    return strtod(buf, NULL);
}

/* parseFloat(string): the longest decimal prefix (after leading whitespace); NaN when there is none. */
static double js_parse_float(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n && ascii_space(s[i])) i++;
    const size_t start = i;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    if (n - i >= 8 && !memcmp(s + i, "Infinity", 8)) return s[start] == '-' ? -INFINITY : INFINITY;
    size_t digits = 0;
    while (i < n && isdigit(s[i])) i++, digits++;
    if (i < n && s[i] == '.') {
        size_t j = i + 1, fd = 0;
        while (j < n && isdigit(s[j])) j++, fd++;
        if (digits + fd > 0) {
            i = j;
            digits += fd;
        }
    }
    if (digits == 0) return NAN;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1, ed = 0;
        if (j < n && (s[j] == '+' || s[j] == '-')) j++;
        while (j < n && isdigit(s[j])) j++, ed++;
        if (ed > 0) i = j;
    }
    char buf[128];
    if (i - start >= sizeof buf) return NAN;
    memcpy(buf, s + start, i - start);
    buf[i - start] = 0;
    return strtod(buf, NULL);
}

/* Number(v) for the values a client sends (undefined: NaN). */
static double js_num(const KrunkerValue *v) {
    if (!v) return NAN;
    switch ((KrunkerValueType)v->type) {
    case KRUNKER_NIL: return 0;
    case KRUNKER_BOOL: return v->u.boolean ? 1 : 0;
    case KRUNKER_NUM: return v->u.num;
    case KRUNKER_STR: return js_str_to_num((const uint8_t *)v->u.str, v->len);
    case KRUNKER_ARRAY:
        /* Number([x]) is Number(String(x)): a one-item array of a number, a string or null */
        if (v->len == 0) return 0;
        if (v->len == 1) {
            const KrunkerValue *x = &v->u.items[0];
            if (x->type == KRUNKER_NIL) return 0;
            if (x->type == KRUNKER_NUM) return x->u.num;
            if (x->type == KRUNKER_STR) return js_str_to_num((const uint8_t *)x->u.str, x->len);
        }
        return NAN;
    default: return NAN;
    }
}

/* Truthiness: 0, NaN, "", null, undefined and false are falsy; objects and arrays are truthy. */
static int js_truthy(const KrunkerValue *v) {
    if (!v) return 0;
    switch ((KrunkerValueType)v->type) {
    case KRUNKER_NIL: return 0;
    case KRUNKER_BOOL: return v->u.boolean != 0;
    case KRUNKER_NUM: return v->u.num != 0 && !isnan(v->u.num);
    case KRUNKER_STR: return v->len > 0;
    default: return 1;
    }
}

int v10_truthy(const KrunkerValue *v) { return js_truthy(v); }

/* ToInt32 (`n | 0`). */
static int32_t js_int32(double d) {
    if (!isfinite(d)) return 0;
    double m = fmod(trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}

/* Number.isInteger(n) && n >= 0 ? n : -1 (storeIndex / cosmeticIndex). */
static int64_t index_of(const KrunkerValue *v) {
    const double n = js_num(v);
    if (isfinite(n) && n == floor(n) && n >= 0) return n >= 9.2e18 ? INT64_MAX : (int64_t)n;
    return -1;
}

/* int32(value, fallback): Number(value) | 0 when finite, else the fallback. */
static int64_t int32_or(const KrunkerValue *v, int64_t fallback) {
    const double n = js_num(v);
    return isfinite(n) ? js_int32(n) : fallback;
}

/* Math.round (ties toward +Infinity). */
static double js_round(double x) {
    const double f = floor(x);
    return (x - f >= 0.5) ? f + 1 : f;
}
static double r1(double n) { return js_round(n * 10) / 10; }
static double r2(double n) { return js_round(n * 100) / 100; }
/* x || 1 */
static double or1(double x) { return (x == 0 || isnan(x)) ? 1 : x; }

/* ---- the v10 output arena */

struct V10Block {
    V10Block *next;
    size_t cap, used;
};
#define V10_BLOCK_DATA(b) ((uint8_t *)((b) + 1))

void *v10_arena(V10Out *o, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!o->blocks || o->blocks->cap - o->blocks->used < n) {
        const size_t cap = n > 4096 ? n : 4096;
        V10Block *b = malloc(sizeof(V10Block) + cap);
        if (!b) {
            o->oom = 1;
            return NULL;
        }
        b->next = o->blocks;
        b->cap = cap;
        b->used = 0;
        o->blocks = b;
    }
    void *p = V10_BLOCK_DATA(o->blocks) + o->blocks->used;
    o->blocks->used += n;
    memset(p, 0, n);
    return p;
}

void v10_out_init(V10Out *o) { memset(o, 0, sizeof *o); }

void v10_out_free(V10Out *o) {
    for (V10Block *b = o->blocks; b;) {
        V10Block *next = b->next;
        free(b);
        b = next;
    }
    free(o->msgs);
    memset(o, 0, sizeof *o);
}

int v10_push(V10Out *o, const char *label, const KrunkerValue *args, uint32_t argc) {
    if (o->count == o->cap) {
        const size_t cap = o->cap ? o->cap * 2 : 8;
        V10Msg *m = realloc(o->msgs, cap * sizeof *m);
        if (!m) {
            o->oom = 1;
            return -1;
        }
        o->msgs = m;
        o->cap = cap;
    }
    KrunkerValue *copy = NULL;
    if (argc) {
        copy = v10_arena(o, argc * sizeof *copy);
        if (!copy) return -1;
        memcpy(copy, args, argc * sizeof *copy);
    }
    o->msgs[o->count++] = (V10Msg){.label = label, .args = copy, .argc = argc};
    return 0;
}

KrunkerValue v10_arr(V10Out *o, const KrunkerValue *items, uint32_t n) {
    KrunkerValue *copy = NULL;
    if (n) {
        copy = v10_arena(o, n * sizeof *copy);
        if (!copy) return KRUNKER_V_NIL;
        memcpy(copy, items, n * sizeof *copy);
    }
    return KRUNKER_V_ARRAY(copy, n);
}

KrunkerValue v10_obj(V10Out *o, const char *const *keys, const KrunkerValue *vals, uint32_t n) {
    KrunkerValue *kv = n ? v10_arena(o, 2 * (size_t)n * sizeof *kv) : NULL;
    if (n && !kv) return KRUNKER_V_NIL;
    for (uint32_t i = 0; i < n; i++) {
        kv[2 * i] = KRUNKER_V_STR(keys[i]);
        kv[2 * i + 1] = vals[i];
    }
    return KRUNKER_V_MAP(kv, n);
}

KrunkerValue v10_text(V10Out *o, const uint8_t *s, size_t n) {
    uint8_t *dst = v10_arena(o, 3 * n + 1);
    if (!dst) return KRUNKER_V_NIL;
    const size_t len = utf8_sanitize(s, n, dst);
    return KRUNKER_V_STRN((const char *)dst, len);
}

/* A text field of a packet: sanitised UTF-8 of the NUL-terminated field, copied into the arena. */
static KrunkerValue field_text(V10Out *o, const uint8_t *p, size_t n, size_t at, size_t size) {
    return v10_text(o, p + at, field_len(p, n, at, size));
}

/* The same field into a NUL-terminated buffer of the sanitised text (cap >= 3 * size + 1). */
static void field_copy(const uint8_t *p, size_t n, size_t at, size_t size, char *dst) {
    uint8_t tmp[3 * 128];
    const size_t len = field_len(p, n, at, size);
    const size_t clean = utf8_sanitize(p + at, len, tmp);
    memcpy(dst, tmp, clean);
    dst[clean] = 0;
}

void v10_hello_payload(uint8_t out[V10_HELLO_PAYLOAD_SIZE]) {
    /* NET_PROTOCOL_MAJOR, MINOR, PATCH (1.4.5: the NetStatePacket layout), NET_HELLO_RULESETS (v10 only). */
    out[0] = NET_PROTOCOL_MAJOR;
    out[1] = NET_PROTOCOL_MINOR;
    out[2] = NET_PROTOCOL_PATCH;
    out[3] = NET_HELLO_RULESETS;
}

/* ---- client -> bridge */

/* ENT (V9) layout (shared.h NetEntPacket): class @0, secondary @4, cosmetics @8 (7 int32: skin colour,
 * hair colour, hat, back, waist, face, dye), spectate @36, slid_cont @37, spray @38, skins @42 (3 int32),
 * lag_comp @54 (float), attach @58, challMode @62, anonMode @63, v7 cosmetics @64 (9 int32: head, back,
 * shoe, pet, wrist, charm[2], player card, badge), show_badges @100. */
enum {
    EN_CLASS = 0, EN_SPRAY = 1, EN_SKINS = 2, EN_HAT = 3, EN_BODY = 4, EN_SECONDARY = 5, EN_SPECTATE = 6,
    EN_CHALLENGE = 7, EN_MELEE = 9, EN_SKIN_COLOR = 10, EN_LAG_COMP = 11, EN_ATTACH = 12, EN_DYE = 14,
    EN_SHOE = 15, EN_WAIST = 16, EN_HAIR = 17, EN_ANON = 19, EN_FACE = 20, EN_PET = 21, EN_VERIFIED = 22,
    EN_PREMIUM = 23, EN_CUSTOM_BADGE = 24, EN_WRIST = 25, EN_CHARMS = 29, EN_BACK = 30, EN_HEAD = 31,
    EN_PLAYER_CARD = 32,
};
#define EN_LENGTH_MIN (EN_SHOE + 1)
#define ENT_COSMETICS_AT 8
#define ENT_SPECTATE_AT 36
#define ENT_SPRAY_AT 38
#define ENT_SKINS_AT 42
#define ENT_LAG_COMP_AT 54
#define ENT_ATTACH_AT 58
#define ENT_CHALL_AT 62
#define ENT_ANON_AT 63
#define ENT_V9_COSMETICS_AT 64
#define ENT_SHOW_BADGES_AT 100
_Static_assert(ENT_SHOW_BADGES_AT + 1 == V10_ENT_SIZE, "the V9 ENT size");

static const int64_t SKIN_COLORS[V10_SKIN_COLORS_N] = {0x805c4a, 0xa77860, 0xd38d6f, 0xcc997e, 0xedbfa6};

/* skinColorFromIndex */
static int64_t skin_color_from_index(const KrunkerValue *v) {
    const double i = js_num(v);
    if (isfinite(i) && i == floor(i) && i >= 0 && i < V10_SKIN_COLORS_N) return SKIN_COLORS[(int)i];
    return -1;
}

/* skinIndexFromColor: the table index of a colour, 0 when it is not in the table. */
static int64_t skin_index_from_color(int64_t color) {
    for (int i = 0; i < V10_SKIN_COLORS_N; i++)
        if (SKIN_COLORS[i] == color) return i;
    return 0;
}

static int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* hairColorFromKrunker: a number > 0 as is, or "#rrggbb" / "rrggbb" (any case); else -1. */
static int64_t hair_color_from_krunker(const KrunkerValue *v) {
    if (v && v->type == KRUNKER_NUM) return v->u.num > 0 ? js_int32(v->u.num) : -1;
    const uint8_t *s = NULL;
    size_t n = 0;
    if (v && v->type == KRUNKER_STR) {
        s = (const uint8_t *)v->u.str;
        n = v->len;
    } else if (v && v->type == KRUNKER_ARRAY && v->len == 1 && v->u.items[0].type == KRUNKER_STR) {
        s = (const uint8_t *)v->u.items[0].u.str; /* String([text]) is the text */
        n = v->u.items[0].len;
    }
    if (!s) return -1;
    if (n == 7 && s[0] == '#') {
        s++;
        n--;
    }
    if (n != 6) return -1;
    int64_t c = 0;
    for (size_t i = 0; i < 6; i++) {
        const int d = hex_digit(s[i]);
        if (d < 0) return -1;
        c = c * 16 + d;
    }
    return c;
}

/* lagCompFromEnt: parseFloat of a string, Number of anything else; 1 when missing or not finite. */
static double lag_comp_from_ent(const KrunkerValue *v) {
    if (!v || v->type == KRUNKER_NIL || (v->type == KRUNKER_STR && v->len == 0)) return 1;
    const double lc = v->type == KRUNKER_STR ? js_parse_float((const uint8_t *)v->u.str, v->len) : js_num(v);
    return isfinite(lc) ? lc : 1;
}

/* An item of a JS array value, or NULL (undefined) past its end or for a non-array. */
static const KrunkerValue *item_of(const KrunkerValue *arr, uint32_t i) {
    if (!arr || arr->type != KRUNKER_ARRAY || i >= arr->len) return NULL;
    return &arr->u.items[i];
}

int v10_encode_enter(const KrunkerValue *en, uint8_t out[V10_ENT_SIZE]) {
    if (!en || en->type != KRUNKER_ARRAY || en->len < EN_LENGTH_MIN) return 0;
    memset(out, 0, V10_ENT_SIZE);
#define IT(i) item_of(en, (i))
    wr_i32(out, 0, int32_or(IT(EN_CLASS), 0));
    wr_i32(out, 4, int32_or(IT(EN_SECONDARY), -1));

    /* cosmeticsFromEnt: the Krunker ent slots (3 hat, 4 back, 10 skinColIndex, 14 dye, 17 waist, 18 hairCol, 21 face) */
    const int64_t skin_color = skin_color_from_index(IT(EN_SKIN_COLOR));
    const int64_t hair_color = hair_color_from_krunker(IT(EN_HAIR));
    wr_i32(out, ENT_COSMETICS_AT + 0, skin_color);
    wr_i32(out, ENT_COSMETICS_AT + 4, hair_color);
    wr_i32(out, ENT_COSMETICS_AT + 8, index_of(IT(EN_HAT)));
    wr_i32(out, ENT_COSMETICS_AT + 12, index_of(IT(EN_BODY)));
    wr_i32(out, ENT_COSMETICS_AT + 16, index_of(IT(EN_WAIST)));
    wr_i32(out, ENT_COSMETICS_AT + 20, index_of(IT(EN_FACE)));
    wr_i32(out, ENT_COSMETICS_AT + 24, index_of(IT(EN_DYE)));

    out[ENT_SPECTATE_AT] = js_truthy(IT(EN_SPECTATE)) ? 1 : 0;
    out[ENT_SPECTATE_AT + 1] = 1; /* slidCont: not in the v10 array, the setting's default */
    wr_i32(out, ENT_SPRAY_AT, int32_or(IT(EN_SPRAY), 0));

    /* skinsFromKrunker: [2] = [primary, secondary] and [9] = meleeIndex */
    wr_i32(out, ENT_SKINS_AT + 0, index_of(item_of(IT(EN_SKINS), 0)));
    wr_i32(out, ENT_SKINS_AT + 4, index_of(item_of(IT(EN_SKINS), 1)));
    wr_i32(out, ENT_SKINS_AT + 8, index_of(IT(EN_MELEE)));
    wr_f32(out, ENT_LAG_COMP_AT, lag_comp_from_ent(IT(EN_LAG_COMP)));

    wr_i32(out, ENT_ATTACH_AT, int32_or(IT(EN_ATTACH), -1));
    out[ENT_CHALL_AT] = js_truthy(IT(EN_CHALLENGE)) ? 1 : 0;
    out[ENT_ANON_AT] = js_truthy(IT(EN_ANON)) ? 1 : 0;

    const KrunkerValue *charms = IT(EN_CHARMS);
    const int64_t v7[9] = {
        index_of(IT(EN_HEAD)),        index_of(IT(EN_BACK)),  index_of(IT(EN_SHOE)),
        index_of(IT(EN_PET)),         index_of(IT(EN_WRIST)), index_of(item_of(charms, 0)),
        index_of(item_of(charms, 1)), index_of(IT(EN_PLAYER_CARD)), -1, /* badge: the server takes the account's */
    };
    for (int i = 0; i < 9; i++) wr_i32(out, ENT_V9_COSMETICS_AT + 4 * (size_t)i, v7[i]);
    out[ENT_SHOW_BADGES_AT] = (js_truthy(IT(EN_VERIFIED)) ? 0x01 : 0) | (js_truthy(IT(EN_PREMIUM)) ? 0x02 : 0) |
                              (js_truthy(IT(EN_CUSTOM_BADGE)) ? 0x08 : 0);
#undef IT
    return 1;
}

/* Code points of the first `max` of a sanitised text, and the byte count they take. */
static size_t chat_prefix_bytes(const uint8_t *s, size_t n, size_t max) {
    size_t i = 0, cps = 0;
    while (i < n && cps < max) {
        size_t l;
        cp_at(s + i, n - i, &l);
        i += l;
        cps++;
    }
    return i;
}

size_t v10_encode_chat(const KrunkerValue *text, uint8_t out[V10_CHAT_TEXT_SIZE]) {
    if (!text || text->type != KRUNKER_STR) return 0;
    uint8_t *clean = malloc(3 * (size_t)text->len + 1);
    if (!clean) return 0;
    const size_t cn = utf8_sanitize((const uint8_t *)text->u.str, text->len, clean);
    size_t i = 0;
    int blank = 1;
    while (i < cn) {
        size_t l;
        if (!js_is_space(cp_at(clean + i, cn - i, &l))) blank = 0;
        i += l;
    }
    size_t keep = 0;
    if (!blank) {
        /* encodeChatSend: the first 70 code points, then at most 287 bytes, then the NUL */
        keep = chat_prefix_bytes(clean, cn, 70);
        if (keep > V10_CHAT_TEXT_SIZE - 1) keep = V10_CHAT_TEXT_SIZE - 1;
        memcpy(out, clean, keep);
    }
    free(clean);
    if (blank) return 0;
    out[keep] = 0;
    return keep + 1;
}

/* ---- native -> v10: the map, the config and the network data */

int v10_parse_map_info(const uint8_t *p, size_t n, V10MapInfo *out) {
    if (n != 44) return 0;
    out->map_index = rd_i32(p, 0);
    out->hash = rd_u32(p, 4);
    out->size = rd_u32(p, 8);
    field_copy(p, n, 12, 32, out->name);
    return 1;
}

void v10_encode_map_request(uint32_t hash, uint8_t out[4]) { wr_i32(out, 0, (int64_t)hash); }

int v10_parse_map_data(const uint8_t *p, size_t n, uint32_t *hash, uint32_t *offset, const uint8_t **data, size_t *dlen) {
    if (n < 10) return 0;
    if (n != 10 + (size_t)rd_u16(p, 8)) return 0;
    *hash = rd_u32(p, 0);
    *offset = rd_u32(p, 4);
    *data = p + 10;
    *dlen = n - 10;
    return 1;
}

int v10_parse_game_config_chunk(const uint8_t *p, size_t n, uint32_t *total, uint32_t *offset, const uint8_t **data, size_t *dlen) {
    if (n < 8) return 0;
    *total = rd_u32(p, 0);
    *offset = rd_u32(p, 4);
    *data = p + 8;
    *dlen = n - 8;
    return 1;
}

int v10_parse_network_data(const uint8_t *p, size_t n, V10Network *out) {
    if (n != 10) return 0;
    out->snf = rd_u16(p, 0);
    out->tf = rd_u16(p, 2);
    out->sf = rd_u16(p, 4);
    return 1;
}

/* NetValuePacket: uint8 flags, char key[71] (TEXT_MAX 70 + NUL), double value. */
#define VALUE_KEY_SIZE 71
#define VALUE_RESET 0x01

int v10_parse_global_value(const uint8_t *p, size_t n, V10GlobalValue *out) {
    if (n != 1 + VALUE_KEY_SIZE + 8) return 0;
    size_t nul = n; /* indexOf(0, 1) */
    for (size_t i = 1; i < n; i++)
        if (!p[i]) {
            nul = i;
            break;
        }
    const size_t end = nul > VALUE_KEY_SIZE ? 1 + VALUE_KEY_SIZE : nul; /* nul < 0 (none) reads as > 71 */
    out->reset = (p[0] & VALUE_RESET) != 0;
    uint8_t tmp[3 * VALUE_KEY_SIZE];
    const size_t clean = utf8_sanitize(p + 1, end - 1, tmp);
    memcpy(out->key, tmp, clean);
    out->key[clean] = 0;
    uint64_t bits = (uint64_t)rd_u32(p, 1 + VALUE_KEY_SIZE) | (uint64_t)rd_u32(p, 1 + VALUE_KEY_SIZE + 4) << 32;
    memcpy(&out->value, &bits, sizeof bits);
    return 1;
}

/* NetPlayerSoundPacket: int32 uid, uint8 add, force, loop, float volume, rate, char name[24]. */
#define SOUND_NAME_SIZE 24

int v10_parse_player_sound(const uint8_t *p, size_t n, V10PlayerSound *out) {
    if (n != 15 + SOUND_NAME_SIZE) return 0;
    out->uid = rd_i32(p, 0);
    out->add = p[4] != 0;
    out->force = p[5] != 0;
    out->loop = p[6] != 0;
    out->volume = rd_f32(p, 7);
    out->rate = rd_f32(p, 11);
    field_copy(p, n, 15, SOUND_NAME_SIZE, out->name);
    return 1;
}

int v10_parse_timer(const uint8_t *p, size_t n, V10Timer *out) {
    if (n != 5) return 0;
    out->seconds = rd_i32(p, 0);
    out->end = p[4] != 0;
    return 1;
}

int v10_parse_init(const uint8_t *p, size_t n, V10Init *out) {
    if (n < 17) return 0;
    out->map_index = rd_i32(p, 3);
    out->mode_index = rd_i32(p, 7);
    out->tick_rate = rd_u16(p, 11);
    out->client_id = rd_i32(p, 13);
    out->ruleset = n > 17 ? p[17] : 0;
    return 1;
}

/* INIT_GAME arguments (buildInitArgs, v10.js 222270). */
void v10_init_game(V10Out *o, const V10Init *init, const uint8_t *map, size_t map_len, KrunkerValue config,
                   KrunkerValue network, int ranked) {
    const KrunkerValue empty = v10_obj(o, NULL, NULL, 0);
    const char *const map_keys[5] = {"data", "id", "creator", "info", "accType"};
    const KrunkerValue map_vals[5] = {KRUNKER_V_STRN((const char *)map, map_len), KRUNKER_V_NIL, KRUNKER_V_STR(""), empty, KRUNKER_V_NUM(1)};
    const KrunkerValue map_obj = v10_obj(o, map_keys, map_vals, 5);
    const KrunkerValue args[V10_INIT_ARGS] = {
        KRUNKER_V_NUM(init->map_index), KRUNKER_V_NUM(init->mode_index), KRUNKER_V_NUM(0), KRUNKER_V_NIL, KRUNKER_V_NIL,
        KRUNKER_V_NIL,                  config,                            KRUNKER_V_NIL, KRUNKER_V_NUM(0), map_obj,
        KRUNKER_V_NIL,                  empty,                             network,       KRUNKER_V_BOOL(1), KRUNKER_V_BOOL(0),
        KRUNKER_V_BOOL(0),              KRUNKER_V_BOOL(ranked),            KRUNKER_V_NIL, KRUNKER_V_NIL,     KRUNKER_V_NIL,
    };
    v10_push(o, V10_IN_INIT_GAME, args, V10_INIT_ARGS);
}

KrunkerValue v10_network_value(V10Out *o, const V10Network *net) {
    static const char *const keys[3] = {"snf", "tf", "sf"};
    const KrunkerValue vals[3] = {KRUNKER_V_NUM(net->snf), KRUNKER_V_NUM(net->tf), KRUNKER_V_NUM(net->sf)};
    return v10_obj(o, keys, vals, 3);
}

/* JSON text -> cJSON (JSON.parse). The null value throws in buildConfig (msg.settings), so it is refused too. */
cJSON *v10_config_parse(const uint8_t *text, size_t len) {
    char *z = malloc(len + 1);
    if (!z) return NULL;
    memcpy(z, text, len);
    z[len] = 0;
    cJSON *root = cJSON_ParseWithOpts(z, NULL, 1); /* JSON.parse: nothing after the value */
    free(z);
    if (root && cJSON_IsNull(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* cJSON value -> v10 value (JSON.parse's values: numbers are doubles, objects keep their key order). */
static KrunkerValue json_value(V10Out *o, const cJSON *j, int depth) {
    if (depth > 200) return KRUNKER_V_NIL;
    if (cJSON_IsNull(j)) return KRUNKER_V_NIL;
    if (cJSON_IsBool(j)) return KRUNKER_V_BOOL(cJSON_IsTrue(j));
    if (cJSON_IsNumber(j)) return KRUNKER_V_NUM(j->valuedouble);
    if (cJSON_IsString(j)) return v10_text(o, (const uint8_t *)j->valuestring, strlen(j->valuestring));
    if (cJSON_IsArray(j)) {
        const int n = cJSON_GetArraySize(j);
        KrunkerValue *items = n ? v10_arena(o, (size_t)n * sizeof *items) : NULL;
        if (n && !items) return KRUNKER_V_NIL;
        int i = 0;
        for (const cJSON *c = j->child; c; c = c->next, i++) items[i] = json_value(o, c, depth + 1);
        return KRUNKER_V_ARRAY(items, (uint32_t)n);
    }
    /* object */
    int n = 0;
    for (const cJSON *c = j->child; c; c = c->next) n++;
    KrunkerValue *kv = n ? v10_arena(o, 2 * (size_t)n * sizeof *kv) : NULL;
    if (n && !kv) return KRUNKER_V_NIL;
    int i = 0;
    for (const cJSON *c = j->child; c; c = c->next, i++) {
        kv[2 * i] = v10_text(o, (const uint8_t *)c->string, strlen(c->string));
        kv[2 * i + 1] = json_value(o, c, depth + 1);
    }
    return KRUNKER_V_MAP(kv, (uint32_t)n);
}

KrunkerValue v10_config_value(V10Out *o, const cJSON *root) {
    /* buildConfig: {...settings} (an object's keys, an array's indices), then config.classes = msg.classes */
    const cJSON *settings = root && cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "settings") : NULL;
    const cJSON *classes = root && cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "classes") : NULL;
    int n = 0;
    if (settings && cJSON_IsObject(settings)) {
        for (const cJSON *c = settings->child; c; c = c->next) n++;
    } else if (settings && cJSON_IsArray(settings)) {
        n = cJSON_GetArraySize(settings);
    }
    const int has_classes = classes && cJSON_IsArray(classes);
    int classes_at = -1;
    if (has_classes) {
        classes_at = -1;
        int i = 0;
        if (settings && cJSON_IsObject(settings))
            for (const cJSON *c = settings->child; c; c = c->next, i++)
                if (!strcmp(c->string, "classes")) classes_at = i;
        if (classes_at < 0) n++;
    }
    KrunkerValue *kv = n ? v10_arena(o, 2 * (size_t)n * sizeof *kv) : NULL;
    if (n && !kv) return KRUNKER_V_NIL;
    int i = 0;
    if (settings && cJSON_IsObject(settings)) {
        for (const cJSON *c = settings->child; c; c = c->next, i++) {
            kv[2 * i] = v10_text(o, (const uint8_t *)c->string, strlen(c->string));
            kv[2 * i + 1] = json_value(o, c, 1);
        }
    } else if (settings && cJSON_IsArray(settings)) {
        for (const cJSON *c = settings->child; c; c = c->next, i++) {
            char key[16];
            snprintf(key, sizeof key, "%d", i);
            kv[2 * i] = v10_text(o, (const uint8_t *)key, strlen(key));
            kv[2 * i + 1] = json_value(o, c, 1);
        }
    }
    if (has_classes) {
        const KrunkerValue val = json_value(o, classes, 1);
        if (classes_at >= 0) {
            kv[2 * classes_at + 1] = val; /* the key keeps its place */
        } else {
            kv[2 * i] = v10_text(o, (const uint8_t *)"classes", 7);
            kv[2 * i + 1] = val;
            i++;
        }
    }
    return KRUNKER_V_MAP(kv, (uint32_t)i);
}

int v10_chunks_init(V10Chunks *c, size_t total) {
    c->buf = NULL;
    c->total = c->end = 0;
    if (total > V10_MAX_CHUNKS_TOTAL) return -1;
    c->buf = malloc(total ? total : 1);
    if (!c->buf) return -1;
    c->total = total;
    return 0;
}

void v10_chunks_free(V10Chunks *c) {
    free(c->buf);
    c->buf = NULL;
    c->total = c->end = 0;
}

int v10_chunks_add(V10Chunks *c, uint32_t offset, const uint8_t *data, size_t n) {
    if ((size_t)offset != c->end || n > c->total - c->end) return 0;
    if (n) memcpy(c->buf + c->end, data, n);
    c->end += n;
    return 1;
}

int v10_chunks_complete(const V10Chunks *c) { return c->end == c->total; }

/* ---- MODE_STATE, OBJECT_STATE, GAME_ANNOUNCEMENT */

/* NetModeStatePacket (little endian): mode_id @0, team_scores[2] @4, flag_info[2][32] @12, active_objective @76,
 * objective_weapon @80, move_lock @84, locked @85, lives @86, flag_count @90, flags[8] (25 bytes each) @91,
 * message[96] @291. */
#define MODE_STATE_SIZE 387
#define MODE_FLAG_SIZE 25
#define MODE_FLAGS_AT 91
#define MODE_MESSAGE_AT 291

void v10_map_mode_state(V10Out *o, const uint8_t *p, size_t n) {
    if (n != MODE_STATE_SIZE) {
        o->stop = "malformed";
        return;
    }
    /* an active objective is sent by MODE_SYNC_V7 (obj needs its time); a weapon objective is wep here */
    if (p[84] != 0) {
        o->stop = "move lock: v10 lm needs the player's position";
        return;
    }
    /* flags in play: ufl(uid, x, y, z, carrier || 0), after the team lines (v10.js 188667 / 188698) */
    KrunkerValue flags[8][5];
    int nflags = 0;
    const int count = p[90] < 8 ? p[90] : 8;
    for (int f = 0; f < count; f++) {
        const size_t at = MODE_FLAGS_AT + (size_t)f * MODE_FLAG_SIZE;
        if (p[at + 24] == 0) continue; /* not in play for this mode */
        const int32_t uid = rd_i32(p, at + 20);
        if (uid < 0) {
            o->stop = "an active flag with no map object id";
            return;
        }
        const int32_t carrier = rd_i32(p, at + 12);
        flags[nflags][0] = KRUNKER_V_NUM(uid);
        flags[nflags][1] = KRUNKER_V_NUM(rd_f32(p, at));
        flags[nflags][2] = KRUNKER_V_NUM(rd_f32(p, at + 4));
        flags[nflags][3] = KRUNKER_V_NUM(rd_f32(p, at + 8));
        flags[nflags][4] = KRUNKER_V_NUM(carrier >= 0 ? carrier : 0);
        nflags++;
    }

    v10_push(o, V10_IN_TEAM_SCORE, (KrunkerValue[]){KRUNKER_V_NUM(1), KRUNKER_V_NUM(rd_i32(p, 4))}, 2);
    v10_push(o, V10_IN_TEAM_SCORE, (KrunkerValue[]){KRUNKER_V_NUM(2), KRUNKER_V_NUM(rd_i32(p, 8))}, 2);
    if (rd_i32(p, 80) >= 0) v10_push(o, V10_IN_SET_WEAPON, (KrunkerValue[]){KRUNKER_V_NUM(rd_i32(p, 80))}, 1);
    if (nflags) {
        v10_push(o, V10_IN_UPDATE_FLAG_INFO, (KrunkerValue[]){KRUNKER_V_NUM(1), field_text(o, p, n, 12, 32)}, 2);
        v10_push(o, V10_IN_UPDATE_FLAG_INFO, (KrunkerValue[]){KRUNKER_V_NUM(2), field_text(o, p, n, 44, 32)}, 2);
        for (int f = 0; f < nflags; f++) v10_push(o, V10_IN_UPDATE_FLAG, flags[f], 5);
    }
    const int32_t lives = rd_i32(p, 86);
    if (lives >= 0) v10_push(o, V10_IN_UPDATE_LIVES, (KrunkerValue[]){KRUNKER_V_NUM(lives)}, 1);
    if (p[85] != 0) v10_push(o, V10_IN_LOCK_PLAYER, (KrunkerValue[]){KRUNKER_V_BOOL(1)}, 1);
    const size_t message_len = field_len(p, n, MODE_MESSAGE_AT, 96);
    if (message_len) v10_push(o, V10_IN_UPDATE_GAME_MESSAGE, (KrunkerValue[]){v10_text(o, p + MODE_MESSAGE_AT, message_len)}, 1);
}

/* PICKUP: NetPickupPacket (26 bytes, packed): int32 uid @0, int32 weapon @4 (-1 empty), uint8 active @8, uint8 moved @9,
 * float position[3] @10, int32 ammo @22. The v10 client reads it as the "inat" pickup half (mapPickup, v10Translate.ts). */
void v10_map_pickup(V10Out *o, const uint8_t *p, size_t n) {
    if (n != 26) {
        o->stop = "malformed";
        return;
    }
    const int32_t uid = rd_i32(p, 0);
    if (uid < 0) {
        o->stop = "a pickup with no map object id";
        return;
    }
    const int32_t weapon = rd_i32(p, 4);
    const int active = p[8] != 0;
    const int moved = p[9] != 0;
    const KrunkerValue pos[3] = {KRUNKER_V_NUM(rd_f32(p, 10)), KRUNKER_V_NUM(rd_f32(p, 14)), KRUNKER_V_NUM(rd_f32(p, 18))};
    const KrunkerValue args[8] = {
        KRUNKER_V_NUM(-1), KRUNKER_V_NIL, KRUNKER_V_NIL, KRUNKER_V_NUM(uid), weapon >= 0 ? KRUNKER_V_NUM(weapon) : KRUNKER_V_NIL,
        KRUNKER_V_BOOL(1), KRUNKER_V_BOOL(active), moved ? v10_arr(o, pos, 3) : KRUNKER_V_NIL,
    };
    v10_push(o, V10_IN_PLAYER_INTERACTION, args, 8);
}

/* NetCheatPacket: uint8 flag (NetCheatFlag, shared/include/shared.h), uint8 on. v10 takes the on / off value as the
 * one argument of each label (v10.js 220244 noc, 220262 unl, 220264 fh, 220250 frz). */
int v10_parse_cheat(const uint8_t *p, size_t n, uint8_t *flag, int *on) {
    if (n != 2 || p[1] > 1) return 0;
    *flag = p[0];
    *on = p[1] != 0;
    return 1;
}

const char *v10_cheat_label(uint8_t flag) {
    switch (flag) {
    case 0: return V10_IN_NOCLIP;
    case 1: return V10_IN_UNLIMITED_AMMO;
    case 2: return V10_IN_FORCE_HIDE;
    case 7: return V10_IN_FROZEN;
    default: return NULL; /* borders (3), forcecol (4), third person (5), held (6): no v10 label, the stop names them */
    }
}

/* NetObjectStatePacket: int32 uid, uint8 kind, int32 value (shared.h NetObjectStateKind). */
void v10_map_object_state(V10Out *o, const uint8_t *p, size_t n) {
    if (n != 9) {
        o->stop = "malformed";
        return;
    }
    const int32_t uid = rd_i32(p, 0);
    const uint8_t kind = p[4];
    const int32_t value = rd_i32(p, 5);
    switch (kind) {
    case 0: v10_push(o, V10_IN_DESTROY_GAMEOBJECT, (KrunkerValue[]){KRUNKER_V_NUM(uid)}, 1); break;
    case 1: v10_push(o, V10_IN_RESPAWN_OBJ, (KrunkerValue[]){KRUNKER_V_NUM(uid)}, 1); break;
    case 2: v10_push(o, V10_IN_UPDATE_GATE, (KrunkerValue[]){KRUNKER_V_NUM(uid), KRUNKER_V_BOOL(value != 0)}, 2); break;
    case 3: v10_push(o, V10_IN_UPDATE_BANK, (KrunkerValue[]){KRUNKER_V_NUM(uid), KRUNKER_V_NUM(value)}, 2); break;
    case 4: v10_push(o, V10_IN_TELEPORT_TO_NODE, (KrunkerValue[]){KRUNKER_V_NUM(uid), KRUNKER_V_NUM(value)}, 2); break;
    case 5: v10_push(o, V10_IN_TELEPORT_FROM_NODE, (KrunkerValue[]){KRUNKER_V_NUM(uid)}, 1); break;
    default: {
        char *why = v10_arena(o, 40);
        if (why) snprintf(why, 40, "object kind %d is not mapped", kind);
        o->stop = why ? why : "object kind is not mapped";
    }
    }
}

/* NetGameAnnouncementPacket (the body after the EXT subtype): char message[256], uint8 ranked_queue,
 * char ranked_label[64]. */
void v10_map_game_announcement(V10Out *o, const uint8_t *body, size_t n) {
    if (n != 256 + 1 + 64) {
        o->stop = "malformed";
        return;
    }
    if (body[256] != 0) {
        o->stop = "the ranked-queue action: its v10 label key is not mapped yet";
        return;
    }
    static const char *const keys[2] = {"message", "actions"};
    const KrunkerValue vals[2] = {field_text(o, body, n, 0, 256), v10_arr(o, NULL, 0)};
    v10_push(o, V10_IN_GAME_ANNOUNCEMENT, (KrunkerValue[]){v10_obj(o, keys, vals, 2)}, 1);
}

/* ---- SKINS, SPAWN, the add record, START, LEADERBOARD, LEADER_STATS */

int v10_parse_skins(const uint8_t *p, size_t n, V10Skins *out) {
    if (n != 16) return 0;
    out->uid = rd_i32(p, 0);
    out->primary = rd_i32(p, 4);
    out->secondary = rd_i32(p, 8);
    out->melee = rd_i32(p, 12);
    return 1;
}

/* NetSpawnPacket, protocol 1.3.1 with the WP-C and WP-F tails (150 bytes): see parseSpawn in v10Translate.ts. */
#define SPAWN_SIZE 150

int v10_parse_spawn(const uint8_t *p, size_t n, V10Spawn *out) {
    if (n != SPAWN_SIZE) return 0;
    out->uid = rd_i32(p, 0);
    out->is_you = p[4] != 0;
    out->active = p[5] != 0;
    out->class_index = rd_i32(p, 6);
    out->team = rd_i32(p, 10);
    out->health = rd_f32(p, 14);
    out->max_health = rd_i32(p, 18);
    for (int k = 0; k < 3; k++) out->position[k] = rd_f32(p, 22 + 4 * (size_t)k);
    for (int k = 0; k < 2; k++) out->direction[k] = rd_f32(p, 34 + 4 * (size_t)k);
    field_copy(p, n, 42, 32, out->name);
    out->secondary_index = rd_i32(p, 74);
    out->skin_color = rd_i32(p, 78);
    out->hair_color = rd_i32(p, 82);
    out->hat = rd_i32(p, 86);
    out->body = rd_i32(p, 90);
    out->waist = rd_i32(p, 94);
    out->face = rd_i32(p, 98);
    out->dye = rd_i32(p, 102);
    out->attach_index = rd_i32(p, 106);
    out->head_mlt = rd_f32(p, 110);
    out->head = rd_i32(p, 114);
    out->back = rd_i32(p, 118);
    out->shoe = rd_i32(p, 122);
    out->pet = rd_i32(p, 126);
    out->wrist = rd_i32(p, 130);
    out->charm[0] = rd_i32(p, 134);
    out->charm[1] = rd_i32(p, 138);
    out->player_card = rd_i32(p, 142);
    out->badge = rd_i32(p, 146);
    return 1;
}

KrunkerValue v10_player_add_record(V10Out *o, const V10Spawn *s, const V10Skins *skins, int has_skins, const char *socket_id) {
    char sid[16];
    KrunkerValue f[V10_ADD_RECORD_FIELDS];
    snprintf(sid, sizeof sid, "%d", s->uid);
    const KrunkerValue pair[2] = {KRUNKER_V_NUM(has_skins ? skins->primary : -1), KRUNKER_V_NUM(has_skins ? skins->secondary : -1)};
    const KrunkerValue charms[2] = {KRUNKER_V_NUM(s->charm[0]), KRUNKER_V_NUM(s->charm[1])};
    const KrunkerValue nil = KRUNKER_V_NIL;

    f[0] = s->is_you ? v10_text(o, (const uint8_t *)socket_id, strlen(socket_id)) : v10_text(o, (const uint8_t *)sid, strlen(sid));
    f[1] = KRUNKER_V_NUM(s->uid);
    f[2] = KRUNKER_V_NUM(js_round(s->position[0]));
    f[3] = KRUNKER_V_NUM(js_round(s->position[1]));
    f[4] = KRUNKER_V_NUM(js_round(s->position[2]));
    f[5] = v10_text(o, (const uint8_t *)s->name, strlen(s->name));
    f[6] = KRUNKER_V_NUM(s->class_index);
    f[7] = KRUNKER_V_NUM(js_round(s->health));
    f[8] = KRUNKER_V_NUM(js_round(s->max_health));
    f[9] = KRUNKER_V_NUM(s->team);
    f[10] = KRUNKER_V_NUM(0); /* level (not in SPAWN) */
    f[11] = KRUNKER_V_NUM(0); /* clan */
    f[12] = v10_arr(o, pair, 2);
    f[13] = KRUNKER_V_NUM(s->hat);
    f[14] = KRUNKER_V_NUM(s->body); /* the v7 ent[4] slot */
    f[15] = s->secondary_index >= 0 ? KRUNKER_V_NUM(s->secondary_index) : nil;
    f[16] = KRUNKER_V_NUM(0); /* hack */
    f[17] = nil;              /* customLoadout */
    f[18] = KRUNKER_V_NUM(s->direction[1]); /* xDire: the yaw (the proxy reads direction[1] the same way) */
    f[19] = KRUNKER_V_NUM(has_skins ? skins->melee : -1);
    f[20] = KRUNKER_V_NUM(s->skin_color < 0 ? -1 : skin_index_from_color(s->skin_color));
    f[21] = KRUNKER_V_NUM(-1); /* propID */
    f[22] = KRUNKER_V_NUM(s->attach_index);
    f[23] = KRUNKER_V_NUM(-1); /* pcStatIndex */
    f[24] = KRUNKER_V_NUM(s->dye);
    f[25] = KRUNKER_V_NUM(0); /* featured */
    f[26] = KRUNKER_V_NUM(-1); /* flg */
    f[27] = KRUNKER_V_NUM(0); /* premium */
    f[28] = f[5];             /* getName */
    f[29] = KRUNKER_V_NUM(s->shoe);
    f[30] = KRUNKER_V_NUM(s->waist);
    f[31] = KRUNKER_V_NUM(0); /* account id */
    if (s->hair_color < 0) {
        f[32] = KRUNKER_V_NUM(0);
    } else {
        char hex[16];
        snprintf(hex, sizeof hex, "#%06llx", (unsigned long long)s->hair_color);
        f[32] = v10_text(o, (const uint8_t *)hex, strlen(hex));
    }
    f[33] = KRUNKER_V_NUM(s->face);
    f[34] = KRUNKER_V_NUM(s->pet);
    f[35] = KRUNKER_V_NUM(s->badge);
    f[36] = KRUNKER_V_NUM(s->wrist);
    f[37] = KRUNKER_V_NUM(0); /* isJuggernaut */
    f[38] = KRUNKER_V_NUM(s->head_mlt);
    f[39] = v10_arr(o, charms, 2);
    f[40] = KRUNKER_V_NUM(0); /* upgrades */
    f[41] = KRUNKER_V_NUM(s->back);
    f[42] = KRUNKER_V_NUM(s->head);
    f[43] = KRUNKER_V_NUM(s->player_card);
    for (int i = 44; i <= 51; i++) f[i] = KRUNKER_V_NUM(0); /* isBot, isBusy, rank, clanId, clanLogo, clan, clanBannerColor, verified */
    f[52] = KRUNKER_V_NUM(-1); /* titleIndex */
    return v10_arr(o, f, V10_ADD_RECORD_FIELDS);
}

void v10_start_game(const V10Spawn *spawn, int32_t game_timer, int32_t chall_mode, KrunkerValue out[8]) {
    out[0] = KRUNKER_V_NUM(game_timer);
    out[1] = KRUNKER_V_NUM(spawn->team);
    out[2] = KRUNKER_V_NUM(0);
    out[3] = KRUNKER_V_NUM(chall_mode);
    out[4] = KRUNKER_V_NUM(0);
    out[5] = KRUNKER_V_NUM(1);
    out[6] = KRUNKER_V_NIL;
    out[7] = KRUNKER_V_NUM(spawn->class_index);
}

/* NetLeaderboardPacket: uint8 count, then 16 rows of int32 uid, score, kills, deaths (257 bytes). */
#define LEADERBOARD_SIZE (1 + 16 * 16)
#define LEADER_MAX 16
#define MODE_ID_FFA 0

void v10_map_leaderboard(V10Out *o, const uint8_t *p, size_t n, int32_t mode_index, int32_t *rows, size_t *count) {
    *count = 0;
    if (n != LEADERBOARD_SIZE) {
        o->stop = "malformed";
        return;
    }
    if (mode_index != MODE_ID_FFA) {
        o->stop = "leaderStats of this mode are not verified (only Free for All)";
        return;
    }
    const size_t rc = p[0] < LEADER_MAX ? p[0] : LEADER_MAX;
    KrunkerValue *vals = rc ? v10_arena(o, rc * 4 * sizeof *vals) : NULL;
    for (size_t i = 0; i < rc; i++) {
        const size_t at = 1 + i * 16;
        for (int k = 0; k < 4; k++) {
            const int32_t v = rd_i32(p, at + 4 * (size_t)k);
            rows[i * 4 + k] = v;
            vals[i * 4 + k] = KRUNKER_V_NUM(v);
        }
    }
    *count = rc * 4;
    v10_push(o, V10_IN_UPDATE_LEADERS, (KrunkerValue[]){v10_arr(o, vals, (uint32_t)(rc * 4))}, 1);
}

/* NetLeaderStatsPacket: uint8 count, uint8 stat_count, int32 uid[16], int32 values[16][4] (322 bytes). */
#define LEADER_STATS_SIZE (2 + 16 * 4 + 16 * 4 * 4)

const char *v10_check_leader_stats(const uint8_t *p, size_t n, const int32_t *rows, size_t row_count, int have_rows) {
    if (n != LEADER_STATS_SIZE) return "malformed";
    if (!have_rows) return "no LEADERBOARD before it";
    const size_t count = p[0] < LEADER_MAX ? p[0] : LEADER_MAX;
    if (p[1] != 3) return "leaderStats of this mode are not verified (only Free for All)";
    if (row_count != count * 4) return "the rows differ from the LEADERBOARD";
    for (size_t i = 0; i < count; i++) {
        if (rd_i32(p, 2 + i * 4) != rows[i * 4]) return "the rows differ from the LEADERBOARD";
        for (size_t k = 0; k < 3; k++)
            if (rd_i32(p, 2 + 16 * 4 + (i * 4 + k) * 4) != rows[i * 4 + 1 + k]) return "the values differ from the LEADERBOARD";
    }
    return NULL;
}

/* ---- MODE_SYNC_V7, DOM_UPDATE, AI_SYNC_V7, AI_ADD_V7, STATE, CHAT_V7 */

/* NetModeSyncV7Packet (65 bytes): team_scores[6] @0, rounds_left @24, has_round_wins @28, team_round_wins[6] @29,
 * lockout_ms @53, objective_uid @57, objective_ms @61. */
#define MODE_SYNC_SIZE 65

void v10_map_mode_sync(V10Out *o, const uint8_t *p, size_t n, int objective_weapon) {
    if (n != MODE_SYNC_SIZE) {
        o->stop = "malformed";
        return;
    }
    for (int t = 1; t <= 5; t++) {
        const int32_t score = rd_i32(p, (size_t)t * 4);
        if (score != 0) v10_push(o, V10_IN_TEAM_SCORE, (KrunkerValue[]){KRUNKER_V_NUM(t), KRUNKER_V_NUM(score)}, 2);
    }
    if (rd_i32(p, 24) != 0) {
        o->stop = "rounds: rds with the round flag false is the round switch (it deactivates every player, v10.js 218999); a join needs another flag, not verified";
        return;
    }
    const int32_t lockout = rd_i32(p, 53);
    if (lockout > 0) v10_push(o, V10_IN_UPDATE_LOCKOUT_TIM, (KrunkerValue[]){KRUNKER_V_NUM(lockout)}, 1);
    const int32_t objective = rd_i32(p, 57);
    if (objective >= 0 && !objective_weapon)
        v10_push(o, V10_IN_SET_OBJECTIVE, (KrunkerValue[]){KRUNKER_V_NUM(objective), KRUNKER_V_NUM(rd_i32(p, 61)), KRUNKER_V_BOOL(1)}, 3);
}

/* NetDomUpdatePacket (497 bytes): uint8 count, then 16 zones of 31 bytes (see mapDomUpdate). */
#define DOM_ZONE_SIZE 31
#define DOM_MAX_ZONES 16
#define DOM_UPDATE_SIZE (1 + DOM_MAX_ZONES * DOM_ZONE_SIZE)

void v10_map_dom_update(V10Out *o, const uint8_t *p, size_t n) {
    if (n != DOM_UPDATE_SIZE) {
        o->stop = "malformed";
        return;
    }
    const size_t count = p[0] < DOM_MAX_ZONES ? p[0] : DOM_MAX_ZONES;
    if (count == 0) return;
    for (size_t z = 0; z < count; z++) {
        if (rd_i32(p, 1 + z * DOM_ZONE_SIZE + 27) < 0) {
            o->stop = "a domination zone with no map object id";
            return;
        }
    }
    KrunkerValue *flat = v10_arena(o, count * 6 * sizeof *flat);
    if (!flat) return;
    for (size_t z = 0; z < count; z++) {
        const size_t at = 1 + z * DOM_ZONE_SIZE;
        const int32_t last_team_on = rd_i32(p, at + 12);
        const size_t teams_n = p[at + 16] < 6 ? p[at + 16] : 6;
        KrunkerValue teams[6];
        for (size_t t = 0; t < teams_n; t++) teams[t] = KRUNKER_V_NUM((int8_t)p[at + 17 + t]);
        KrunkerValue *f = flat + z * 6;
        f[0] = KRUNKER_V_NUM(rd_i32(p, at + 27));
        f[1] = KRUNKER_V_NUM(rd_i32(p, at + 4));
        f[2] = KRUNKER_V_NUM(rd_f32(p, at + 8));
        f[3] = last_team_on ? KRUNKER_V_NUM(last_team_on) : KRUNKER_V_NIL;
        f[4] = v10_arr(o, teams, (uint32_t)teams_n);
        f[5] = KRUNKER_V_NUM(or1(rd_f32(p, at + 23)));
    }
    v10_push(o, V10_IN_UPDATE_DOM_ZONE, (KrunkerValue[]){KRUNKER_V_ARRAY(flat, (uint32_t)(count * 6))}, 1);
}

/* NetAISyncV7Packet (498 bytes): uint8 first @0, uint8 count @1, then 16 entries of 31 bytes. */
#define AI_SYNC_ENTRY_SIZE 31
#define AI_SYNC_MAX 16

int v10_parse_bot_sync_chunk(const uint8_t *p, size_t n, V10BotChunk *out) {
    const int count = n >= 2 ? p[1] : -1;
    if (count < 0 || count > AI_SYNC_MAX || n != 2 + (size_t)count * AI_SYNC_ENTRY_SIZE) return 0;
    out->first = p[0] != 0;
    out->count = (size_t)count;
    for (int i = 0; i < count; i++) {
        const size_t at = 2 + (size_t)i * AI_SYNC_ENTRY_SIZE;
        V10BotEntry *e = &out->entries[i];
        e->sid = rd_i32(p, at);
        e->x = r1(rd_f32(p, at + 4));
        e->y = r1(rd_f32(p, at + 8));
        e->z = r1(rd_f32(p, at + 12));
        e->xd = r2(rd_f32(p, at + 16));
        e->yd = r2(rd_f32(p, at + 20));
        e->health = (int32_t)floor((double)rd_i32(p, at + 24));
        e->did_move = p[at + 28] != 0;
        e->team = rd_i16(p, at + 29);
    }
    return 1;
}

KrunkerValue v10_bot_sync_list(V10Out *o, const V10BotEntry *entries, size_t count) {
    KrunkerValue *v = count ? v10_arena(o, count * 9 * sizeof *v) : NULL;
    for (size_t i = 0; i < count; i++) {
        KrunkerValue *f = v + i * 9;
        f[0] = KRUNKER_V_NUM(entries[i].sid);
        f[1] = KRUNKER_V_NUM(entries[i].x);
        f[2] = KRUNKER_V_NUM(entries[i].y);
        f[3] = KRUNKER_V_NUM(entries[i].z);
        f[4] = KRUNKER_V_NUM(entries[i].xd);
        f[5] = KRUNKER_V_NUM(entries[i].yd);
        f[6] = KRUNKER_V_NUM(entries[i].health);
        f[7] = KRUNKER_V_BOOL(entries[i].did_move);
        f[8] = KRUNKER_V_NUM(entries[i].team);
    }
    return KRUNKER_V_ARRAY(v, (uint32_t)(count * 9));
}

/* NetAIAddV7Packet (packed, 237 bytes): see mapBotAdd in v10Translate.ts for the field offsets. */
#define AI_ADD_V7_SIZE 237

void v10_map_bot_add(V10Out *o, const uint8_t *p, size_t n) {
    if (n != AI_ADD_V7_SIZE) {
        o->stop = "malformed";
        return;
    }
    KrunkerValue dat = KRUNKER_V_NUM(0);
    if (p[60] != 0) {
        static const char *const keys[28] = {"hasAnim", "animS", "soundR", "team", "health", "mSize", "mYOff", "mROff",
                                             "hbh", "asset", "idl", "src", "noVis", "tagScale", "lvl", "side",
                                             "opacity", "alphaTest", "texEncode", "noSmooth", "noFog", "fTrans",
                                             "frames", "frameT", "rAnim", "rAnimA", "bobD", "bobS"};
        const KrunkerValue vals[28] = {
            KRUNKER_V_NUM(p[61] ? 1 : 0),
            KRUNKER_V_NUM(rd_f32(p, 68)),
            KRUNKER_V_NUM(rd_f32(p, 72)),
            KRUNKER_V_NUM(rd_i16(p, 66)),
            KRUNKER_V_NUM(rd_f32(p, 76)),
            KRUNKER_V_NUM(rd_f32(p, 80)),
            KRUNKER_V_NUM(rd_f32(p, 84)),
            KRUNKER_V_NUM(rd_f32(p, 88)),
            KRUNKER_V_NUM(r1(or1(rd_f32(p, 92)))),
            field_text(o, p, n, 96, 16),
            KRUNKER_V_NUM(rd_i32(p, 112)),
            field_text(o, p, n, 116, 24),
            KRUNKER_V_NUM(p[62] ? 1 : 0),
            KRUNKER_V_NUM(or1(rd_f32(p, 140))),
            KRUNKER_V_NUM(rd_i16(p, 144)),
            KRUNKER_V_NUM(p[146]),
            KRUNKER_V_NUM(rd_f32(p, 147)),
            KRUNKER_V_NUM(rd_f32(p, 151)),
            KRUNKER_V_NUM(p[155]),
            KRUNKER_V_BOOL(p[65] != 0),
            KRUNKER_V_BOOL(p[63] != 0),
            KRUNKER_V_BOOL(p[64] != 0),
            KRUNKER_V_NUM(p[156]),
            KRUNKER_V_NUM(rd_f32(p, 157)),
            KRUNKER_V_NUM(rd_f32(p, 161)),
            KRUNKER_V_NUM(p[165]),
            KRUNKER_V_NUM(rd_f32(p, 166)),
            KRUNKER_V_NUM(rd_f32(p, 170)),
        };
        dat = v10_obj(o, keys, vals, 28);
    }
    const KrunkerValue args[7] = {
        KRUNKER_V_NUM(rd_i32(p, 0)),
        KRUNKER_V_NUM(rd_i32(p, 4)),
        field_text(o, p, n, 8, 40),
        KRUNKER_V_NUM(r1(rd_f32(p, 48))),
        KRUNKER_V_NUM(r1(rd_f32(p, 52))),
        KRUNKER_V_NUM(r1(rd_f32(p, 56))),
        dat,
    };
    v10_push(o, V10_IN_C_SPAWN, args, 7);
}

/* NetStatePacket (protocol 1.4.5, 79 bytes): see mapState in v10Translate.ts. */
#define STATE_SIZE 79

int v10_map_state(const uint8_t *p, size_t n, V10StateRec *out) {
    if (n != STATE_SIZE) return 0;
    const double rad = 180.0 / 3.14159265358979323846; /* 180 / Math.PI */
    out->uid = rd_i32(p, 0);
    out->x = r2(rd_f32(p, 13));
    out->y = r2(rd_f32(p, 17));
    out->z = r2(rd_f32(p, 21));
    out->x_dire = js_round(rd_f32(p, 41) * rad); /* the yaw */
    out->y_dire = js_round(rd_f32(p, 37) * rad); /* the pitch */
    out->step_val = rd_f32(p, 57);
    out->on_ground = p[61] != 0;
    out->crouch = js_round(rd_f32(p, 53));
    out->loadout = rd_i32(p, 9);
    out->aim = rd_f32(p, 49) != 0; /* NaN !== 0 is true, as in the TS */
    out->grapple_on = p[62] != 0;
    for (int k = 0; k < 3; k++) out->grapple[k] = js_round(rd_f32(p, 63 + 4 * (size_t)k));
    out->ping = js_round(rd_f32(p, 75));
    return 1;
}

KrunkerValue v10_state_record(V10Out *o, const V10StateRec *r) {
    KrunkerValue f[V10_STATE_FIELDS];
    const KrunkerValue grapple[3] = {KRUNKER_V_NUM(r->grapple[0]), KRUNKER_V_NUM(r->grapple[1]), KRUNKER_V_NUM(r->grapple[2])};
    f[0] = KRUNKER_V_NUM(r->uid);
    f[1] = KRUNKER_V_NUM(r->x);
    f[2] = KRUNKER_V_NUM(r->y);
    f[3] = KRUNKER_V_NUM(r->z);
    f[4] = KRUNKER_V_NUM(r->x_dire);
    f[5] = KRUNKER_V_NUM(r->y_dire);
    f[6] = KRUNKER_V_NUM(r->step_val);
    f[7] = KRUNKER_V_NUM(r->on_ground ? 1 : 0);
    f[8] = KRUNKER_V_NUM(r->crouch);
    f[9] = KRUNKER_V_NUM(r->loadout);
    f[10] = KRUNKER_V_NUM(r->aim ? 1 : 0);
    f[11] = r->grapple_on ? v10_arr(o, grapple, 3) : KRUNKER_V_NUM(0);
    f[12] = KRUNKER_V_NUM(r->ping);
    return v10_arr(o, f, V10_STATE_FIELDS);
}

/* CHAT_V7 (330 bytes): int8 channel @0, int32 account @1, then NetChatPacket: int32 uid @5, uint8 kind @9,
 * name[32] @10, text[288] @42. */
#define CHAT_V7_SIZE 330

KrunkerValue v10_escape_html(V10Out *o, const uint8_t *s, size_t n) {
    uint8_t *clean = malloc(3 * n + 1);
    if (!clean) {
        o->oom = 1;
        return KRUNKER_V_NIL;
    }
    const size_t cn = utf8_sanitize(s, n, clean);
    size_t total = 0;
    for (size_t i = 0; i < cn; i++) {
        switch (clean[i]) {
        case '&': total += 5; break;
        case '<': case '>': total += 4; break;
        case '"': total += 6; break;
        case '\'': total += 6; break;
        default: total += 1;
        }
    }
    uint8_t *out = v10_arena(o, total + 1);
    if (out) {
        size_t w = 0;
        for (size_t i = 0; i < cn; i++) {
            const char *rep = NULL;
            switch (clean[i]) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#x27;"; break;
            default: break;
            }
            if (rep) {
                memcpy(out + w, rep, strlen(rep));
                w += strlen(rep);
            } else {
                out[w++] = clean[i];
            }
        }
    }
    KrunkerValue v = out ? KRUNKER_V_STRN((const char *)out, total) : KRUNKER_V_NIL;
    free(clean);
    return v;
}

/* The text of the found-junk line: /^You found \[([0-9]+\.[0-9]{2})\] Junk$/ (the amount is copied). */
static int junk_amount(const uint8_t *s, size_t n, const uint8_t **amount, size_t *alen) {
    static const char pre[] = "You found [";
    static const char post[] = "] Junk";
    const size_t pl = sizeof pre - 1, ql = sizeof post - 1;
    if (n < pl + ql + 4 || memcmp(s, pre, pl) || memcmp(s + n - ql, post, ql)) return 0;
    const uint8_t *a = s + pl;
    const size_t an = n - pl - ql;
    size_t i = 0;
    while (i < an && isdigit(a[i])) i++;
    if (i == 0 || i + 3 != an || a[i] != '.' || !isdigit(a[i + 1]) || !isdigit(a[i + 2])) return 0;
    *amount = a;
    *alen = an;
    return 1;
}

void v10_map_chat_v7(V10Out *o, const uint8_t *p, size_t n) {
    if (n != CHAT_V7_SIZE) {
        o->stop = "malformed";
        return;
    }
    const uint8_t kind = p[9];
    const size_t raw_len = field_len(p, n, 42, V10_CHAT_TEXT_SIZE);
    /* KN kind 3 (NET_CHAT_FOUND_JUNK) is the finder's "You found [x.xx] Junk": v10 builds that line itself */
    if (kind == 3) {
        uint8_t clean[3 * V10_CHAT_TEXT_SIZE];
        const size_t cn = utf8_sanitize(p + 42, raw_len, clean);
        const uint8_t *amount = NULL;
        size_t alen = 0;
        if (!junk_amount(clean, cn, &amount, &alen)) {
            o->stop = "a found-junk line that is not the v10 form";
            return;
        }
        const char *a = "<span style='color:#fff'>You</span> found <span style='color:#cc51f8'>[";
        const char *b = "]</span> Junk";
        const size_t total = strlen(a) + alen + strlen(b);
        uint8_t *html = v10_arena(o, total + 1);
        if (!html) return;
        memcpy(html, a, strlen(a));
        memcpy(html + strlen(a), amount, alen);
        memcpy(html + strlen(a) + alen, b, strlen(b));
        v10_push(o, V10_IN_ADD_CHAT, (KrunkerValue[]){KRUNKER_V_NUM(-1), KRUNKER_V_NIL, KRUNKER_V_STRN((const char *)html, total), KRUNKER_V_NUM(1)}, 4);
        return;
    }
    /* KN kind 4 (NET_CHAT_RANKED): the ranked queue's notice in the ranked green (v10's direct line) */
    if (kind == 4) {
        const KrunkerValue text = v10_escape_html(o, p + 42, raw_len);
        const KrunkerValue args[5] = {KRUNKER_V_NUM(-1), KRUNKER_V_NIL, text, KRUNKER_V_NUM(2), KRUNKER_V_STR("#5ce05a")};
        v10_push(o, V10_IN_ADD_CHAT, args, 5);
        return;
    }
    if (kind > 2) {
        char *why = v10_arena(o, 40);
        if (why) snprintf(why, 40, "chat kind %d has no v10 kind", kind);
        o->stop = why ? why : "chat kind has no v10 kind";
        return;
    }
    const KrunkerValue name = kind == 2 ? KRUNKER_V_NIL : v10_escape_html(o, p + 10, field_len(p, n, 10, 32));
    const KrunkerValue text = v10_escape_html(o, p + 42, raw_len);
    const KrunkerValue args[8] = {
        KRUNKER_V_NUM((int8_t)p[0]), name, text, KRUNKER_V_NUM(kind), KRUNKER_V_NIL, KRUNKER_V_NIL,
        KRUNKER_V_NUM(rd_i32(p, 1)), KRUNKER_V_NIL,
    };
    v10_push(o, V10_IN_ADD_CHAT, args, 8);
}
