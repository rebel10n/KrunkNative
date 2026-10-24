// KrunkScript network messages on an official krunker.io session (shared/include/ks_net.h). deob-10.0.3.js:
//   GAME.NETWORK.send / broadcast (:209434-209469): the client sends C2S "sd" (CUSTOM_NETWORK_DATA) [id, data],
//   the server S2C "sd" (CUSTOM_NETWORK_MESSAGE) [id, data] -> mainScript.onNetworkMessage(id, data) (:241493).
// `data` is the script's own JS value, msgpack-encoded by the socket: a KrunkScript object keeps its compiled
// keys, "V_" + name (iíiìiîî.prefix :189986), so {a: 1} travels as {V_a: 1}. These convert between that value
// and the body of our NET_PACKET_SCRIPT_MSG (uint8 id_len, id, ks_serialize(data)).
#include <ks_net.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFFICIAL_MAX_DEPTH 100 // @msgpack/msgpack's maxDepth (krunker_proto.h)

typedef struct {
    uint8_t *out;
    size_t cap, len;
    int failed;
} Enc;

static void put(Enc *e, const void *p, const size_t n) {
    if (e->len + n > e->cap) {
        e->failed = 1;
        return;
    }
    memcpy(e->out + e->len, p, n);
    e->len += n;
}
static void put_u8(Enc *e, const uint8_t b) { put(e, &b, 1); }
static void put_varint(Enc *e, uint64_t v) {
    while (v >= 0x80) {
        put_u8(e, (uint8_t) (v | 0x80));
        v >>= 7;
    }
    put_u8(e, (uint8_t) v);
}
static void put_str(Enc *e, const char *p, const size_t n) {
    put_varint(e, n);
    put(e, p, n);
}

// String(number) for the integer keys and ids the decoder gives as numbers
static size_t num_string(const double d, char *buf, const size_t n) {
    if (isfinite(d) && d == floor(d) && fabs(d) < 1e21) snprintf(buf, n, "%.0f", d);
    else if (isnan(d)) snprintf(buf, n, "NaN");
    else if (isinf(d)) snprintf(buf, n, d > 0 ? "Infinity" : "-Infinity");
    else snprintf(buf, n, "%.17g", d);
    return strlen(buf);
}

static void enc_value(Enc *e, const KrunkerValue *v, const int depth) {
    if (e->failed) return;
    if (depth > OFFICIAL_MAX_DEPTH) {
        e->failed = 1;
        return;
    }
    switch (v ? v->type : KRUNKER_NIL) {
        case KRUNKER_BOOL: put_u8(e, v->u.boolean ? 3 : 2); return;
        case KRUNKER_NUM: {
            put_u8(e, 4);
            uint64_t bits;
            memcpy(&bits, &v->u.num, 8);
            uint8_t b[8];
            for (int i = 0; i < 8; i++) b[i] = (uint8_t) (bits >> (8 * i));
            put(e, b, 8);
            return;
        }
        case KRUNKER_STR: put_u8(e, 5); put_str(e, v->u.str, v->len); return;
        case KRUNKER_ARRAY:
            put_u8(e, 6);
            put_varint(e, v->len);
            for (uint32_t i = 0; i < v->len && !e->failed; i++) enc_value(e, &v->u.items[i], depth + 1);
            return;
        case KRUNKER_MAP:
            put_u8(e, 7);
            put_varint(e, v->len);
            for (uint32_t i = 0; i < v->len && !e->failed; i++) {
                const KrunkerValue *k = &v->u.items[i * 2];
                char buf[40];
                const char *key = buf;
                size_t key_len;
                if (k->type == KRUNKER_STR) {
                    key = k->u.str;
                    key_len = k->len;
                    if (key_len >= 2 && key[0] == 'V' && key[1] == '_') key += 2, key_len -= 2; // prefix.remove
                } else {
                    key_len = num_string(k->type == KRUNKER_NUM ? k->u.num : 0, buf, sizeof(buf)); // the decoder's number keys
                }
                put_str(e, key, key_len);
                enc_value(e, &v->u.items[i * 2 + 1], depth + 1);
            }
            return;
        default: put_u8(e, 1); return; // nil (null / undefined), and the JS-only Uint8Array / Date: null
    }
}

int ks_net_body_from_official(const KrunkerValue *id, const KrunkerValue *data, uint8_t *body, const size_t cap) {
    if (!body || cap < 2) return -1;
    char buf[40];
    const char *id_str = "";
    size_t id_len = 0;
    if (id && id->type == KRUNKER_STR) {
        id_str = id->u.str;
        id_len = id->len;
    } else if (id && id->type == KRUNKER_NUM) {
        id_len = num_string(id->u.num, buf, sizeof(buf));
        id_str = buf;
    }
    if (id_len > KS_NET_ID_MAX || memchr(id_str, 0, id_len)) return -1;
    Enc e = {body, cap, 0, 0};
    put_u8(&e, (uint8_t) id_len);
    put(&e, id_str, id_len);
    enc_value(&e, data, 1);
    return e.failed ? -1 : (int) e.len;
}

// ---------------------------------------------------------------- body -> msgpack value

typedef struct {
    const uint8_t *in;
    size_t len, pos;
    int ok;
    KsNetOfficial *pool;
} Dec;

static void *pool_alloc(KsNetOfficial *p, const size_t n) {
    if (p->block_count == p->block_cap) {
        const size_t cap = p->block_cap ? p->block_cap * 2 : 16;
        void **blocks = realloc(p->blocks, cap * sizeof(void *));
        if (!blocks) return NULL;
        p->blocks = blocks;
        p->block_cap = cap;
    }
    void *b = calloc(1, n ? n : 1);
    if (b) p->blocks[p->block_count++] = b;
    return b;
}

static int get_varint(Dec *d, uint64_t *v) {
    uint64_t r = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (d->pos >= d->len) return 0;
        const uint8_t b = d->in[d->pos++];
        r |= (uint64_t) (b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *v = r;
            return 1;
        }
    }
    return 0;
}

// a string of the body, NUL-terminated in the pool, `prefix` ("V_" for keys) in front
static const char *get_str(Dec *d, const char *prefix, uint32_t *len) {
    uint64_t n;
    if (!get_varint(d, &n) || n > d->len - d->pos) {
        d->ok = 0;
        return NULL;
    }
    const size_t pre = prefix ? strlen(prefix) : 0;
    char *s = pool_alloc(d->pool, pre + (size_t) n + 1);
    if (!s) {
        d->ok = 0;
        return NULL;
    }
    if (pre) memcpy(s, prefix, pre);
    memcpy(s + pre, d->in + d->pos, (size_t) n);
    d->pos += (size_t) n;
    *len = (uint32_t) (pre + n);
    return s;
}

static void dec_value(Dec *d, KrunkerValue *out, const int depth) {
    memset(out, 0, sizeof(*out));
    if (!d->ok || d->pos >= d->len || depth > OFFICIAL_MAX_DEPTH) {
        d->ok = 0;
        return;
    }
    const uint8_t tag = d->in[d->pos++];
    switch (tag) {
        case 0:
        case 1: out->type = KRUNKER_NIL; return; // undefined and null: msgpack nil
        case 2:
        case 3:
            out->type = KRUNKER_BOOL;
            out->u.boolean = tag == 3;
            return;
        case 4: {
            if (d->len - d->pos < 8) {
                d->ok = 0;
                return;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 8; i++) bits |= (uint64_t) d->in[d->pos + i] << (8 * i);
            d->pos += 8;
            out->type = KRUNKER_NUM;
            memcpy(&out->u.num, &bits, 8);
            return;
        }
        case 5:
            out->type = KRUNKER_STR;
            out->u.str = get_str(d, NULL, &out->len);
            return;
        case 6:
        case 7: {
            uint64_t n;
            if (!get_varint(d, &n) || n > d->len - d->pos) {
                d->ok = 0;
                return;
            }
            const size_t slots = (size_t) n * (tag == 7 ? 2 : 1);
            KrunkerValue *items = pool_alloc(d->pool, slots * sizeof(KrunkerValue));
            if (!items) {
                d->ok = 0;
                return;
            }
            out->type = tag == 7 ? KRUNKER_MAP : KRUNKER_ARRAY;
            out->len = (uint32_t) n;
            out->u.items = items;
            for (uint64_t i = 0; i < n && d->ok; i++) {
                if (tag == 7) { // a KrunkerScript key: "V_" + name on the wire (prefix.add)
                    items[i * 2].type = KRUNKER_STR;
                    items[i * 2].u.str = get_str(d, "V_", &items[i * 2].len);
                    dec_value(d, &items[i * 2 + 1], depth + 1);
                } else {
                    dec_value(d, &items[i], depth + 1);
                }
            }
            return;
        }
        default: d->ok = 0; return;
    }
}

int ks_net_body_to_official(const uint8_t *body, const size_t len, char *id, const size_t id_cap, KsNetOfficial *out) {
    if (!body || len < 1 || !id || !id_cap || !out) return 0;
    memset(out, 0, sizeof(*out));
    const size_t id_len = body[0];
    if (id_len > KS_NET_ID_MAX || 1 + id_len > len) return 0;
    const size_t copy = id_len < id_cap - 1 ? id_len : id_cap - 1;
    memcpy(id, body + 1, copy);
    id[copy] = 0;
    Dec d = {body, len, 1 + id_len, 1, out};
    dec_value(&d, &out->data, 1);
    if (!d.ok || d.pos != len) {
        ks_net_official_free(out);
        return 0;
    }
    return 1;
}

void ks_net_official_free(KsNetOfficial *v) {
    if (!v) return;
    for (size_t i = 0; i < v->block_count; i++) free(v->blocks[i]);
    free(v->blocks);
    memset(v, 0, sizeof(*v));
}
