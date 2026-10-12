// KrunkScript network message helpers (shared/include/ks_net.h, doc/v7/krunkscript.md 2.5-2.8).
#include <ks_net.h>

#include <stdlib.h>
#include <string.h>

int ks_net_encode_msg(KsScript *s, const char *id, const KsValue data, uint8_t *body, const size_t cap)
{
    if (!s || !id || !body || cap < 2) return -1;
    const size_t id_len = strlen(id);
    if (id_len > KS_NET_ID_MAX) return -1;
    // JSON.stringify(data).length > 2000 (objects / arrays, measured with the V_ prefixes) or
    // (data + "").length for primitives; a throwing stringify (cycles) rejects too.
    const size_t json = ks_json_length_prefixed(s, data);
    if (json == (size_t) -1 || json > KS_NET_JSON_MAX) return -1;
    body[0] = (uint8_t) id_len;
    if (1 + id_len > cap) return -1;
    memcpy(body + 1, id, id_len);
    const size_t room = cap - 1 - id_len;
    const size_t n = ks_serialize(s, data, body + 1 + id_len, room);
    if (!n || n > room) return -1;
    return (int) (1 + id_len + n);
}

int ks_net_decode_msg(KsScript *s, const uint8_t *body, const size_t len, char *id, const size_t id_cap, KsValue *data)
{
    if (!s || !body || len < 1 || !id || !id_cap) return 0;
    const size_t id_len = body[0];
    if (id_len > KS_NET_ID_MAX || 1 + id_len > len) return 0;
    const size_t copy = id_len < id_cap - 1 ? id_len : id_cap - 1;
    memcpy(id, body + 1, copy);
    id[copy] = 0;
    if (memchr(id, 0, copy)) return 0; // an embedded NUL cannot round-trip as a C string
    int ok = 0;
    const KsValue v = ks_deserialize(s, body + 1 + id_len, len - 1 - id_len, &ok);
    if (!ok) return 0;
    if (data) *data = v;
    return 1;
}

int ks_net_chunk_count(const size_t body_len)
{
    if (!body_len) return 1;
    return (int) ((body_len + KS_NET_CHUNK_DATA - 1) / KS_NET_CHUNK_DATA);
}

uint16_t ks_net_make_chunk(const uint8_t *body, const size_t len, const uint16_t seq, const int i, uint8_t *out)
{
    const int count = ks_net_chunk_count(len);
    if (i < 0 || i >= count || count > 255) return 0;
    const size_t offset = (size_t) i * KS_NET_CHUNK_DATA;
    const size_t n = len - offset < (size_t) KS_NET_CHUNK_DATA ? len - offset : (size_t) KS_NET_CHUNK_DATA;
    const KsNetChunkHeader header = {.msg_seq = seq, .chunk = (uint8_t) i, .chunk_count = (uint8_t) count};
    memcpy(out, &header, sizeof(header));
    if (n) memcpy(out + sizeof(header), body + offset, n);
    return (uint16_t) (sizeof(header) + n);
}

int ks_net_reassemble(KsNetReassembler *r, const void *payload, const uint16_t len, const uint8_t **body, size_t *body_len)
{
    if (!r || !payload || len < sizeof(KsNetChunkHeader)) return -1;
    KsNetChunkHeader h;
    memcpy(&h, payload, sizeof(h));
    const uint8_t *data = (const uint8_t *) payload + sizeof(h);
    const size_t n = len - sizeof(h);
    if (!h.chunk_count || h.chunk >= h.chunk_count || n > (size_t) KS_NET_CHUNK_DATA ||
        (size_t) h.chunk_count * KS_NET_CHUNK_DATA > KS_NET_MAX_BODY + KS_NET_CHUNK_DATA) {
        r->count = 0;
        return -1;
    }
    if (h.chunk == 0) { // a new message (drops an unfinished one)
        r->seq = h.msg_seq;
        r->count = h.chunk_count;
        r->next = 0;
        r->len = 0;
    } else if (!r->count || h.msg_seq != r->seq || h.chunk != r->next || h.chunk_count != r->count) {
        r->count = 0;
        return -1;
    }
    // every chunk but the last is full
    if (h.chunk + 1 < h.chunk_count && n != (size_t) KS_NET_CHUNK_DATA) {
        r->count = 0;
        return -1;
    }
    if (r->len + n > KS_NET_MAX_BODY) {
        r->count = 0;
        return -1;
    }
    if (r->len + n > r->cap) {
        size_t cap = r->cap ? r->cap : 1024;
        while (cap < r->len + n) cap *= 2;
        uint8_t *buf = realloc(r->buf, cap);
        if (!buf) {
            r->count = 0;
            return -1;
        }
        r->buf = buf;
        r->cap = cap;
    }
    if (n) memcpy(r->buf + r->len, data, n);
    r->len += n;
    r->next++;
    if (r->next < r->count) return 0;
    r->count = 0;
    if (body) *body = r->buf;
    if (body_len) *body_len = r->len;
    return 1;
}

void ks_net_reassembler_free(KsNetReassembler *r)
{
    if (!r) return;
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

int ks_net_send_msg(KsScript *s, uint16_t *seq, const char *id, const KsValue data, const KsNetSendFn send, void *user)
{
    if (!send) return 0;
    uint8_t *body = malloc(KS_NET_MAX_BODY);
    if (!body) return 0;
    const int len = ks_net_encode_msg(s, id, data, body, KS_NET_MAX_BODY);
    if (len < 0) {
        free(body);
        return 0;
    }
    const uint16_t msg_seq = seq ? (*seq)++ : 0;
    const int count = ks_net_chunk_count((size_t) len);
    uint8_t chunk[NET_MAX_PAYLOAD];
    int ok = 1;
    for (int i = 0; i < count && ok; i++) {
        const uint16_t n = ks_net_make_chunk(body, (size_t) len, msg_seq, i, chunk);
        ok = n && send(user, NET_PACKET_SCRIPT_MSG, chunk, n);
    }
    free(body);
    return ok;
}

// Cuts at a UTF-8 boundary at most `max` bytes in.
static size_t utf8_cut(const char *text, size_t len, size_t max)
{
    if (len <= max) return len;
    size_t n = max;
    while (n > 0 && ((unsigned char) text[n] & 0xC0) == 0x80) n--;
    return n;
}

uint16_t ks_net_encode_log(const int is_error, const char *text, uint8_t *out)
{
    if (!text) text = "";
    size_t len = strlen(text);
    out[0] = (uint8_t) !!is_error;
    if (len > KS_NET_LOG_TEXT_MAX) {
        const size_t n = utf8_cut(text, len, KS_NET_LOG_TEXT_MAX - 3);
        memcpy(out + 1, text, n);
        memcpy(out + 1 + n, "...", 3);
        return (uint16_t) (1 + n + 3);
    }
    memcpy(out + 1, text, len);
    return (uint16_t) (1 + len);
}

int ks_net_decode_log(const void *payload, const uint16_t len, int *is_error, char *text, const size_t cap)
{
    if (!payload || len < 1 || len > 1 + KS_NET_LOG_TEXT_MAX || !text || !cap) return 0;
    const uint8_t *p = payload;
    if (p[0] > 1) return 0;
    if (is_error) *is_error = p[0];
    size_t n = len - 1u;
    if (n > cap - 1) n = utf8_cut((const char *) p + 1, n, cap - 1);
    memcpy(text, p + 1, n);
    text[n] = 0;
    for (size_t i = 0; i < n; i++)
        if (!text[i]) text[i] = ' ';
    return 1;
}

int ks_net_apply_sync(KsScript *s, const KsValue self, const void *payload, const uint16_t len)
{
    if (!s || !payload || !len) return -1;
    int ok = 0;
    const KsValue list = ks_deserialize(s, payload, len, &ok);
    if (!ok || list.tag != KS_ARR) return -1;
    if (self.tag != KS_OBJ && self.tag != KS_HOST) return 0;
    const size_t n = ks_arr_len(list);
    int written = 0;
    char key[128];
    for (size_t i = 0; i + 1 < n; i += 2) {
        const KsValue name = ks_arr_get(s, list, i);
        size_t key_len = 0;
        const char *k = ks_str_utf8(name, &key_len);
        if (!k || !key_len || key_len >= sizeof(key) || memchr(k, 0, key_len)) continue;
        memcpy(key, k, key_len);
        key[key_len] = 0;
        // IDX:14769-14775: only a property the client already holds, with a non-object value
        const KsValue current = ks_obj_get(s, self, key);
        if (current.tag == KS_UNDEF || current.tag == KS_NULL || current.tag == KS_OBJ || current.tag == KS_ARR ||
            current.tag == KS_HOST)
            continue;
        ks_obj_set(s, self, key, ks_arr_get(s, list, i + 1));
        written++;
    }
    return written;
}
