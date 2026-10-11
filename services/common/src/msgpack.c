/* knsvc/msgpack.h implementation (MessagePack spec 2.0 subset: no timestamp ext semantics). */
#include "knsvc/msgpack.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MP_MAX_DEPTH 64

/* ------------------------------------------------------------------ writer */

static int put_be(kn_buf *b, uint8_t tag, uint64_t v, int bytes) {
    uint8_t tmp[9];
    tmp[0] = tag;
    for (int i = 0; i < bytes; i++) tmp[1 + i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
    return kn_buf_append(b, tmp, (size_t)bytes + 1);
}

int kn_mp_write_nil(kn_buf *b) { return kn_buf_append_byte(b, 0xc0); }
int kn_mp_write_bool(kn_buf *b, int v) { return kn_buf_append_byte(b, v ? 0xc3 : 0xc2); }

int kn_mp_write_uint(kn_buf *b, uint64_t v) {
    if (v < 128) return kn_buf_append_byte(b, (uint8_t)v);
    if (v <= 0xff) return put_be(b, 0xcc, v, 1);
    if (v <= 0xffff) return put_be(b, 0xcd, v, 2);
    if (v <= 0xffffffffu) return put_be(b, 0xce, v, 4);
    return put_be(b, 0xcf, v, 8);
}

int kn_mp_write_int(kn_buf *b, int64_t v) {
    if (v >= 0) return kn_mp_write_uint(b, (uint64_t)v);
    if (v >= -32) return kn_buf_append_byte(b, (uint8_t)(int8_t)v);
    if (v >= INT8_MIN) return put_be(b, 0xd0, (uint64_t)v & 0xff, 1);
    if (v >= INT16_MIN) return put_be(b, 0xd1, (uint64_t)v & 0xffff, 2);
    if (v >= INT32_MIN) return put_be(b, 0xd2, (uint64_t)v & 0xffffffffu, 4);
    return put_be(b, 0xd3, (uint64_t)v, 8);
}

int kn_mp_write_float(kn_buf *b, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    return put_be(b, 0xca, u, 4);
}

int kn_mp_write_double(kn_buf *b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    return put_be(b, 0xcb, u, 8);
}

int kn_mp_write_str(kn_buf *b, const char *s, size_t len) {
    int r;
    if (len > 0xffffffffu) return -1;
    if (len < 32) r = kn_buf_append_byte(b, (uint8_t)(0xa0 | len));
    else if (len <= 0xff) r = put_be(b, 0xd9, len, 1);
    else if (len <= 0xffff) r = put_be(b, 0xda, len, 2);
    else r = put_be(b, 0xdb, len, 4);
    return r ? r : kn_buf_append(b, s, len);
}

int kn_mp_write_cstr(kn_buf *b, const char *s) { return kn_mp_write_str(b, s, strlen(s)); }

int kn_mp_write_bin(kn_buf *b, const void *p, size_t len) {
    int r;
    if (len > 0xffffffffu) return -1;
    if (len <= 0xff) r = put_be(b, 0xc4, len, 1);
    else if (len <= 0xffff) r = put_be(b, 0xc5, len, 2);
    else r = put_be(b, 0xc6, len, 4);
    return r ? r : kn_buf_append(b, p, len);
}

int kn_mp_write_array(kn_buf *b, uint32_t count) {
    if (count < 16) return kn_buf_append_byte(b, (uint8_t)(0x90 | count));
    if (count <= 0xffff) return put_be(b, 0xdc, count, 2);
    return put_be(b, 0xdd, count, 4);
}

int kn_mp_write_map(kn_buf *b, uint32_t count) {
    if (count < 16) return kn_buf_append_byte(b, (uint8_t)(0x80 | count));
    if (count <= 0xffff) return put_be(b, 0xde, count, 2);
    return put_be(b, 0xdf, count, 4);
}

/* ------------------------------------------------------------------ reader */

void kn_mp_reader_init(kn_mp_reader *r, const void *data, size_t len) {
    r->p = data;
    r->len = len;
    r->pos = 0;
    r->error = 0;
}

static int need(kn_mp_reader *r, size_t n) {
    if (r->error || r->len - r->pos < n) {
        r->error = 1;
        return -1;
    }
    return 0;
}

static uint64_t get_be(kn_mp_reader *r, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | r->p[r->pos + (size_t)i];
    r->pos += (size_t)bytes;
    return v;
}

static int read_len(kn_mp_reader *r, int bytes, uint32_t *out) {
    if (need(r, (size_t)bytes)) return -1;
    *out = (uint32_t)get_be(r, bytes);
    return 0;
}

static int read_blob(kn_mp_reader *r, kn_mp_value *v, kn_mp_type t, uint32_t len) {
    if (need(r, len)) return -1;
    v->type = t;
    v->v.str.ptr = (const char *)r->p + r->pos;
    v->v.str.len = len;
    r->pos += len;
    return 0;
}

static int read_ext(kn_mp_reader *r, kn_mp_value *v, uint32_t len) {
    if (need(r, 1)) return -1;
    v->ext_type = (int8_t)r->p[r->pos++];
    return read_blob(r, v, KN_MP_EXT, len);
}

static int read_signed(kn_mp_reader *r, kn_mp_value *v, int bytes) {
    if (need(r, (size_t)bytes)) return -1;
    uint64_t u = get_be(r, bytes);
    int shift = 64 - 8 * bytes;
    v->type = KN_MP_INT;
    v->v.i = shift ? (int64_t)(u << shift) >> shift : (int64_t)u;
    return 0;
}

static int read_float(kn_mp_reader *r, kn_mp_value *v, int bytes) {
    if (need(r, (size_t)bytes)) return -1;
    uint64_t u = get_be(r, bytes);
    v->type = KN_MP_FLOAT;
    if (bytes == 4) {
        uint32_t u32 = (uint32_t)u;
        float f;
        memcpy(&f, &u32, 4);
        v->v.f = f;
    } else {
        memcpy(&v->v.f, &u, 8);
    }
    return 0;
}

int kn_mp_read(kn_mp_reader *r, kn_mp_value *v) {
    memset(v, 0, sizeof *v);
    if (need(r, 1)) return -1;
    uint8_t c = r->p[r->pos++];
    uint32_t n;
    if (c <= 0x7f) { v->type = KN_MP_UINT; v->v.u = c; return 0; }
    if (c >= 0xe0) { v->type = KN_MP_INT; v->v.i = (int8_t)c; return 0; }
    if ((c & 0xf0) == 0x80) { v->type = KN_MP_MAP; v->v.count = c & 15; return 0; }
    if ((c & 0xf0) == 0x90) { v->type = KN_MP_ARRAY; v->v.count = c & 15; return 0; }
    if ((c & 0xe0) == 0xa0) return read_blob(r, v, KN_MP_STR, c & 31);
    switch (c) {
    case 0xc0: v->type = KN_MP_NIL; return 0;
    case 0xc2: case 0xc3: v->type = KN_MP_BOOL; v->v.b = c == 0xc3; return 0;
    case 0xc4: case 0xc5: case 0xc6:
        return read_len(r, 1 << (c - 0xc4), &n) ? -1 : read_blob(r, v, KN_MP_BIN, n);
    case 0xc7: case 0xc8: case 0xc9:
        return read_len(r, 1 << (c - 0xc7), &n) ? -1 : read_ext(r, v, n);
    case 0xca: return read_float(r, v, 4);
    case 0xcb: return read_float(r, v, 8);
    case 0xcc: case 0xcd: case 0xce: case 0xcf:
        if (need(r, (size_t)1 << (c - 0xcc))) return -1;
        v->type = KN_MP_UINT;
        v->v.u = get_be(r, 1 << (c - 0xcc));
        return 0;
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: return read_signed(r, v, 1 << (c - 0xd0));
    case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8: return read_ext(r, v, 1u << (c - 0xd4));
    case 0xd9: case 0xda: case 0xdb:
        return read_len(r, 1 << (c - 0xd9), &n) ? -1 : read_blob(r, v, KN_MP_STR, n);
    case 0xdc: case 0xdd:
        if (read_len(r, c == 0xdc ? 2 : 4, &v->v.count)) return -1;
        v->type = KN_MP_ARRAY;
        return 0;
    case 0xde: case 0xdf:
        if (read_len(r, c == 0xde ? 2 : 4, &v->v.count)) return -1;
        v->type = KN_MP_MAP;
        return 0;
    default: /* 0xc1 never used */
        r->error = 1;
        return -1;
    }
}

static int skip_depth(kn_mp_reader *r, int depth) {
    kn_mp_value v;
    if (depth > MP_MAX_DEPTH || kn_mp_read(r, &v)) return r->error = 1, -1;
    uint64_t children = v.type == KN_MP_ARRAY ? v.v.count : v.type == KN_MP_MAP ? (uint64_t)v.v.count * 2 : 0;
    /* each child needs >= 1 byte: reject impossible counts early */
    if (children > r->len - r->pos) return r->error = 1, -1;
    for (uint64_t i = 0; i < children; i++)
        if (skip_depth(r, depth + 1)) return -1;
    return 0;
}

int kn_mp_skip(kn_mp_reader *r) { return skip_depth(r, 0); }

/* ------------------------------------------------------------------ cJSON bridge */

static int json_to_mp(const cJSON *j, kn_buf *out, int depth) {
    if (depth > MP_MAX_DEPTH) return -1;
    if (cJSON_IsNull(j) || cJSON_IsInvalid(j)) return kn_mp_write_nil(out);
    if (cJSON_IsBool(j)) return kn_mp_write_bool(out, cJSON_IsTrue(j));
    if (cJSON_IsNumber(j)) {
        double d = j->valuedouble;
        /* msgpack-lite rule: int32-representable values as ints, everything else float64 */
        if (isfinite(d) && d == floor(d) && d >= -2147483648.0 && d <= 2147483647.0)
            return kn_mp_write_int(out, (int64_t)d);
        return kn_mp_write_double(out, d);
    }
    if (cJSON_IsString(j)) return kn_mp_write_cstr(out, j->valuestring ? j->valuestring : "");
    if (cJSON_IsRaw(j)) return kn_mp_write_cstr(out, j->valuestring ? j->valuestring : "");
    uint32_t n = (uint32_t)cJSON_GetArraySize(j);
    if (cJSON_IsArray(j)) {
        if (kn_mp_write_array(out, n)) return -1;
        for (const cJSON *c = j->child; c; c = c->next)
            if (json_to_mp(c, out, depth + 1)) return -1;
        return 0;
    }
    if (cJSON_IsObject(j)) {
        if (kn_mp_write_map(out, n)) return -1;
        for (const cJSON *c = j->child; c; c = c->next)
            if (kn_mp_write_cstr(out, c->string ? c->string : "") || json_to_mp(c, out, depth + 1)) return -1;
        return 0;
    }
    return -1;
}

int kn_msgpack_from_json(const cJSON *json, kn_buf *out) {
    size_t start = out->len;
    if (json_to_mp(json, out, 0) == 0) return 0;
    out->len = start;
    if (out->data) out->data[start] = 0;
    return -1;
}

static cJSON *string_node(const char *p, uint32_t len) {
    char tmp[256];
    char *s = len < sizeof tmp ? tmp : malloc((size_t)len + 1);
    if (!s) return NULL;
    memcpy(s, p, len);
    s[len] = 0;
    cJSON *n = cJSON_CreateString(s);
    if (s != tmp) free(s);
    return n;
}

static cJSON *mp_to_json(kn_mp_reader *r, int depth);

static int key_text(kn_mp_reader *r, char *out, size_t cap, int depth) {
    kn_mp_reader save = *r;
    kn_mp_value k;
    if (kn_mp_read(r, &k)) return -1;
    if (k.type == KN_MP_STR || k.type == KN_MP_BIN) {
        size_t n = k.v.str.len < cap - 1 ? k.v.str.len : cap - 1;
        memcpy(out, k.v.str.ptr, n);
        out[n] = 0;
        return 0;
    }
    if (k.type == KN_MP_UINT) return snprintf(out, cap, "%llu", (unsigned long long)k.v.u), 0;
    if (k.type == KN_MP_INT) return snprintf(out, cap, "%lld", (long long)k.v.i), 0;
    /* other key types: stringify via JSON */
    *r = save;
    cJSON *j = mp_to_json(r, depth + 1);
    if (!j) return -1;
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return -1;
    snprintf(out, cap, "%s", s);
    free(s);
    return 0;
}

static cJSON *mp_to_json(kn_mp_reader *r, int depth) {
    kn_mp_value v;
    if (depth > MP_MAX_DEPTH || kn_mp_read(r, &v)) return NULL;
    switch (v.type) {
    case KN_MP_NIL: return cJSON_CreateNull();
    case KN_MP_BOOL: return cJSON_CreateBool(v.v.b);
    case KN_MP_INT: return cJSON_CreateNumber((double)v.v.i);
    case KN_MP_UINT: return cJSON_CreateNumber((double)v.v.u);
    case KN_MP_FLOAT: return cJSON_CreateNumber(v.v.f);
    case KN_MP_STR: case KN_MP_BIN: return string_node(v.v.str.ptr, v.v.str.len);
    case KN_MP_EXT: return cJSON_CreateNull();
    case KN_MP_ARRAY: {
        if (v.v.count > r->len - r->pos) return NULL;
        cJSON *a = cJSON_CreateArray();
        for (uint32_t i = 0; a && i < v.v.count; i++) {
            cJSON *c = mp_to_json(r, depth + 1);
            if (!c) { cJSON_Delete(a); return NULL; }
            cJSON_AddItemToArray(a, c);
        }
        return a;
    }
    case KN_MP_MAP: {
        if ((uint64_t)v.v.count * 2 > r->len - r->pos) return NULL;
        cJSON *o = cJSON_CreateObject();
        for (uint32_t i = 0; o && i < v.v.count; i++) {
            char key[512];
            if (key_text(r, key, sizeof key, depth)) { cJSON_Delete(o); return NULL; }
            cJSON *c = mp_to_json(r, depth + 1);
            if (!c) { cJSON_Delete(o); return NULL; }
            /* duplicate keys: last wins (like msgpack-lite) */
            if (cJSON_GetObjectItemCaseSensitive(o, key)) cJSON_ReplaceItemInObjectCaseSensitive(o, key, c);
            else cJSON_AddItemToObject(o, key, c);
        }
        return o;
    }
    }
    return NULL;
}

cJSON *kn_msgpack_to_json(const void *data, size_t len, size_t *consumed) {
    kn_mp_reader r;
    kn_mp_reader_init(&r, data, len);
    cJSON *j = mp_to_json(&r, 0);
    if (j && consumed) *consumed = r.pos;
    return j;
}

/* ------------------------------------------------------------------ Krunker framing */

int kn_krunker_packet_encode(const cJSON *packet, kn_buf *out) {
    if (kn_msgpack_from_json(packet, out) != 0) return -1;
    static const uint8_t trailer[2] = {0, 0};
    return kn_buf_append(out, trailer, 2);
}

cJSON *kn_krunker_packet_decode(const void *data, size_t len) {
    if (len < 3) return NULL;
    const uint8_t *p = data;
    if (p[len - 1] != 0 || p[len - 2] != 0) return NULL;
    size_t used = 0;
    cJSON *j = kn_msgpack_to_json(data, len - 2, &used);
    if (j && used != len - 2) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}
