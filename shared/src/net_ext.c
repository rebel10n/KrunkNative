#include <string.h>
#include <net_ext.h>

// NET_PACKET_EXT envelope (shared/include/net_ext.h): u16 subtype, little-endian, then the body.

uint16_t net_ext_pack(uint8_t *out, const size_t out_size, const uint16_t subtype, const void *body, const uint16_t body_length) {
    if (!out || body_length > NET_EXT_BODY_MAX || out_size < (size_t) NET_EXT_HEADER_SIZE + body_length) return 0;
    out[0] = (uint8_t) (subtype & 0xff);
    out[1] = (uint8_t) (subtype >> 8);
    if (body_length) memcpy(out + NET_EXT_HEADER_SIZE, body, body_length);
    return (uint16_t) (NET_EXT_HEADER_SIZE + body_length);
}

int net_ext_unpack(const void *payload, const uint16_t length, uint16_t *subtype, const uint8_t **body, uint16_t *body_length) {
    if (!payload || length < NET_EXT_HEADER_SIZE) return 0;
    const uint8_t *p = payload;
    if (subtype) *subtype = (uint16_t) (p[0] | (p[1] << 8));
    if (body) *body = p + NET_EXT_HEADER_SIZE;
    if (body_length) *body_length = (uint16_t) (length - NET_EXT_HEADER_SIZE);
    return 1;
}
