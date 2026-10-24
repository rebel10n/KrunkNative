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

// v10's player clip list (deob-10.0.3.js:21165): PLAYER_ANIM's clipIndex is a position in it (players.playClip :193810)
static const char *const k_player_clips[NET_PLAYER_CLIP_COUNT] = {"Idle", "Move", "MoveFast", "Jump", "Melee", "Shoot", "Crouch"};

const char *net_player_clip_name(const int clip) {
    return clip >= 0 && clip < NET_PLAYER_CLIP_COUNT ? k_player_clips[clip] : NULL;
}

int net_player_clip_index(const char *name) {
    for (int i = 0; name && i < NET_PLAYER_CLIP_COUNT; i++) {
        if (!strcmp(k_player_clips[i], name)) return i;
    }
    return -1;
}
