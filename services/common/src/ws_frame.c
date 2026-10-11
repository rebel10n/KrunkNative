/* RFC 6455 framing primitives from knsvc/ws.h. */
#include "knsvc/crypto.h"
#include "knsvc/ws.h"

#include <string.h>

int kn_ws_frame_parse(const uint8_t *d, size_t len, kn_ws_frame *f) {
    memset(f, 0, sizeof *f);
    if (len < 2) return 0;
    if (d[0] & 0x70) return -1; /* RSV1-3: no extensions negotiated */
    f->fin = (d[0] & 0x80) != 0;
    f->opcode = d[0] & 0x0f;
    int op = f->opcode;
    if (!(op == 0 || op == 1 || op == 2 || op == 8 || op == 9 || op == 10)) return -1;
    f->masked = (d[1] & 0x80) != 0;
    uint64_t plen = d[1] & 0x7f;
    size_t pos = 2;
    if (plen == 126) {
        if (len < 4) return 0;
        plen = (uint64_t)d[2] << 8 | d[3];
        if (plen < 126) return -1; /* non-minimal */
        pos = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = plen << 8 | d[2 + i];
        if (plen >> 63) return -1;
        if (plen <= 0xffff) return -1; /* non-minimal */
        pos = 10;
    }
    if (op >= 8 && (!f->fin || plen > 125)) return -1; /* control frames */
    if (f->masked) {
        if (len < pos + 4) return 0;
        memcpy(f->mask, d + pos, 4);
        pos += 4;
    }
    f->payload_len = plen;
    f->header_len = pos;
    return 1;
}

void kn_ws_mask(uint8_t *data, size_t len, const uint8_t mask[4], size_t offset) {
    for (size_t i = 0; i < len; i++) data[i] ^= mask[(i + offset) & 3];
}

int kn_ws_frame_encode(kn_buf *out, int opcode, int fin, const void *payload, size_t len, const uint8_t mask[4]) {
    uint8_t h[14];
    size_t hl = 2;
    h[0] = (uint8_t)((fin ? 0x80 : 0) | (opcode & 0x0f));
    uint8_t mbit = mask ? 0x80 : 0;
    if (len < 126) {
        h[1] = (uint8_t)(mbit | len);
    } else if (len <= 0xffff) {
        h[1] = mbit | 126;
        h[2] = (uint8_t)(len >> 8);
        h[3] = (uint8_t)len;
        hl = 4;
    } else {
        h[1] = mbit | 127;
        for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        hl = 10;
    }
    if (mask) {
        memcpy(h + hl, mask, 4);
        hl += 4;
    }
    if (kn_buf_reserve(out, hl + len) != 0) return -1;
    kn_buf_append(out, h, hl);
    if (len) {
        size_t at = out->len;
        kn_buf_append(out, payload, len);
        if (mask) kn_ws_mask((uint8_t *)out->data + at, len, mask, 0);
    }
    return 0;
}

int kn_ws_accept_key(const char *client_key, char out[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char buf[128];
    size_t kl = strlen(client_key);
    if (kl + sizeof guid > sizeof buf) return -1;
    memcpy(buf, client_key, kl);
    memcpy(buf + kl, guid, sizeof guid - 1);
    uint8_t sha[KN_SHA1_LEN];
    if (kn_sha1(buf, kl + sizeof guid - 1, sha) != 0) return -1;
    return kn_base64_encode(sha, sizeof sha, out, 29, 0, 1) == 28 ? 0 : -1;
}

int kn_utf8_valid(const uint8_t *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t n;
        uint32_t cp;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 0x07; }
        else return 0;
        if (n >= len - i) return 0;
        for (size_t k = 1; k <= n; k++) {
            if ((s[i + k] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (s[i + k] & 0x3f);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff))
            return 0;
        i += n + 1;
    }
    return 1;
}
