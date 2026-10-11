#include <shared.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

// Optional trailing fields: older peers send the prefix, the readers below fill the rest with defaults.
_Static_assert(offsetof(NetEntPacket, cosmetics) == NET_ENT_PACKET_V2_SIZE, "ENT cosmetics must follow secondary_index");
_Static_assert(offsetof(NetSpawnPacket, cosmetics) == NET_SPAWN_PACKET_V1_SIZE, "SPAWN cosmetics must follow secondary_index");
_Static_assert(sizeof(NetEntPacket) == NET_ENT_PACKET_V3_SIZE + 1, "ENT spectate must be the last byte");
_Static_assert(NET_INPUT_PACKET_V1_SIZE == 22 && sizeof(NetInputPacket) == 23, "INPUT slowdown must be the last byte");

#ifdef WIN32
#include <winsock2.h>
#define NET_SEND(fd, data, len) send((fd), (const char*) (data), (int) (len), 0)
#define NET_ERRNO WSAGetLastError()
#define NET_WOULD_BLOCK(err) ((err) == WSAEWOULDBLOCK)
#else
#include <sys/socket.h>
#include <errno.h>
// A peer that closed its socket must not kill the process with SIGPIPE: Linux
// takes MSG_NOSIGNAL per call, macOS/BSD set SO_NOSIGPIPE on the socket instead.
#ifdef MSG_NOSIGNAL
#define NET_SEND(fd, data, len) send((fd), (data), (len), MSG_NOSIGNAL)
#else
#define NET_SEND(fd, data, len) net_send_nosigpipe((fd), (data), (len))
static ssize_t net_send_nosigpipe(const int fd, const void *data, const size_t length) {
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return send(fd, data, length, 0);
}
#endif
#define NET_ERRNO errno
#define NET_WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#endif

int net_send_packet(const int fd, const NetPacketType type, const void *payload, const uint16_t length) {
    if (length > NET_MAX_PAYLOAD) return 0;

    uint8_t frame[NET_PACKET_HEADER_SIZE + NET_MAX_PAYLOAD];
    frame[0] = (uint8_t) type;
    frame[1] = (uint8_t) (length >> 8);
    frame[2] = (uint8_t) (length & 0xff);

    if (payload && length) memcpy(frame + NET_PACKET_HEADER_SIZE, payload, length);

    size_t sent = 0;
    const size_t total = NET_PACKET_HEADER_SIZE + length;

    while (sent < total) {
        const int wrote = NET_SEND(fd, frame + sent, total - sent);

        if (wrote <= 0) {
            if (wrote < 0 && NET_WOULD_BLOCK(NET_ERRNO)) continue;
            return 0;
        }

        sent += (size_t) wrote;
    }

    return 1;
}

int net_buffer_push(NetPacketBuffer *buffer, const void *data, const size_t length) {
    if (!length) return 1;
    if (length > sizeof(buffer->data) - buffer->length) return 0;

    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;

    return 1;
}

int net_buffer_next(NetPacketBuffer *buffer, NetPacket *packet) {
    if (buffer->length < NET_PACKET_HEADER_SIZE) return 0;

    const uint16_t length = (uint16_t) (((uint16_t) buffer->data[1] << 8) | buffer->data[2]);
    if (length > NET_MAX_PAYLOAD) return -1;

    const size_t frame_length = NET_PACKET_HEADER_SIZE + length;
    if (buffer->length < frame_length) return 0;

    packet->type = buffer->data[0];
    packet->length = length;
    if (length) memcpy(packet->payload, buffer->data + NET_PACKET_HEADER_SIZE, length);

    buffer->length -= frame_length;
    if (buffer->length) memmove(buffer->data, buffer->data + frame_length, buffer->length);

    return 1;
}

void net_packet_from_input(NetInputPacket *packet, const Input *input) {
    packet->seq = input->seq;
    packet->move_dir = input->move_dir;
    packet->delta = input->delta;
    packet->x_dir = input->x_dir;
    packet->y_dir = input->y_dir;
    packet->flags = 0;
    packet->swap = input->swap;
    const float slowdown = roundf(input->slowdown * 10.0f);
    packet->slowdown = (uint8_t) (slowdown <= 0.0f ? 0 : slowdown >= 10.0f ? 10 : slowdown);

    if (input->shoot) packet->flags |= NET_INPUT_SHOOT;
    if (input->scope) packet->flags |= NET_INPUT_SCOPE;
    if (input->jump) packet->flags |= NET_INPUT_JUMP;
    if (input->crouch) packet->flags |= NET_INPUT_CROUCH;
    if (input->reload) packet->flags |= NET_INPUT_RELOAD;
}

void net_packet_to_input(Input *input, const NetInputPacket *packet) {
    memset(input, 0, sizeof(*input));

    input->seq = packet->seq;
    input->move_dir = packet->move_dir;
    input->delta = packet->delta;
    input->x_dir = packet->x_dir;
    input->y_dir = packet->y_dir;
    input->swap = packet->swap;
    input->slowdown = (packet->slowdown > 10 ? 10 : packet->slowdown) / 10.0f;
    input->shoot = !!(packet->flags & NET_INPUT_SHOOT);
    input->scope = !!(packet->flags & NET_INPUT_SCOPE);
    input->jump = !!(packet->flags & NET_INPUT_JUMP);
    input->crouch = !!(packet->flags & NET_INPUT_CROUCH);
    input->reload = !!(packet->flags & NET_INPUT_RELOAD);
}

void net_packet_from_player(NetStatePacket *packet, const Player *player) {
    packet->uid = player->uid;
    packet->ack_seq = player->input_seq;
    packet->active = player->active;
    packet->loadout_index = player->loadout_index;
    packet->position[0] = player->position.x;
    packet->position[1] = player->position.y;
    packet->position[2] = player->position.z;
    packet->velocity[0] = player->velocity.x;
    packet->velocity[1] = player->velocity.y;
    packet->velocity[2] = player->velocity.z;
    packet->direction[0] = player->direction.x;
    packet->direction[1] = player->direction.y;
    packet->health = player->health;
    packet->aim_val = player->aim_val;
    packet->crouch_val = player->crouch_val;
}

void net_packet_apply_player(Player *player, const NetStatePacket *packet) {
    player->active = packet->active;
    player->input_seq = packet->ack_seq;
    player->position.x = packet->position[0];
    player->position.y = packet->position[1];
    player->position.z = packet->position[2];
    player->velocity.x = packet->velocity[0];
    player->velocity.y = packet->velocity[1];
    player->velocity.z = packet->velocity[2];
    player->direction.x = packet->direction[0];
    player->direction.y = packet->direction[1];
    player->health = packet->health;
    player->aim_val = packet->aim_val;
    player->crouch_val = packet->crouch_val;

    if (packet->loadout_index >= 0 && packet->loadout_index < player->loadout_size && packet->loadout_index != player->loadout_index) {
        player_swap_weapon(player, packet->loadout_index, 0, 1, 1);
    }
}

// Krunker v3.1.1 chat input: the page caps #chatInput at 70 characters and the
// server drops blank lines. Copies at most NET_CHAT_MAX_CHARS code points of `in`
// (stopping at a NUL), replaces control characters with spaces, drops invalid
// UTF-8 bytes and trims the ends. Returns 1 when something is left.
int net_chat_sanitize(char *out, const size_t size, const char *in, const size_t length) {
    if (!out || !size) return 0;
    out[0] = 0;
    if (!in) return 0;

    size_t written = 0;
    int chars = 0;
    size_t i = 0;

    while (i < length && in[i] && chars < NET_CHAT_MAX_CHARS) {
        const unsigned char c = (unsigned char) in[i];
        size_t seq = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;

        if (!seq || i + seq > length) { i++; continue; }

        int valid = 1;
        for (size_t k = 1; k < seq; k++) {
            if (((unsigned char) in[i + k] & 0xc0) != 0x80) valid = 0;
        }
        if (!valid) { i++; continue; }
        if (written + seq + 1 > size) break;

        if (seq == 1) out[written++] = (char) (c < 0x20 || c == 0x7f ? ' ' : c);
        else for (size_t k = 0; k < seq; k++) out[written++] = in[i + k];

        i += seq;
        chars++;
    }

    out[written] = 0;

    size_t start = 0;
    while (out[start] == ' ') start++;
    while (written > start && out[written - 1] == ' ') out[--written] = 0;
    if (start) memmove(out, out + start, written - start + 1);

    return out[0] != 0;
}

// io_ct burst limit: a window opens on the first message and allows
// NET_CHAT_BURST_MAX messages within NET_CHAT_BURST_WINDOW seconds.
int net_chat_allow(NetChatLimiter *limiter, const float now) {
    if (!limiter->count || now - limiter->window_start >= NET_CHAT_BURST_WINDOW) {
        limiter->window_start = now;
        limiter->count = 1;
    } else {
        limiter->count++;
    }

    return limiter->count <= NET_CHAT_BURST_MAX;
}

void net_packet_chat(NetChatPacket *packet, const int uid, const NetChatKind kind, const char *name, const char *text) {
    memset(packet, 0, sizeof(*packet));
    packet->uid = uid;
    packet->kind = (uint8_t) kind;
    if (name) strncpy(packet->name, name, sizeof(packet->name) - 1);
    if (text) strncpy(packet->text, text, sizeof(packet->text) - 1);
}

// io_po: one RTT sample folded into the published ping (EWMA, alpha 0.35).
int net_ping_update(const int previous_ms, const float rtt_seconds) {
    int sample = (int) (rtt_seconds * 1000.0f + 0.5f);
    if (sample < 0) sample = 0;
    if (sample > NET_PING_MAX_MS) sample = NET_PING_MAX_MS;
    if (previous_ms < 0) return sample;

    const float smoothed = (float) previous_ms + (float) (sample - previous_ms) * NET_PING_EWMA_ALPHA;
    return (int) (smoothed + (smoothed >= 0.0f ? 0.5f : -0.5f));
}

static uint32_t net_fnv1a(const char *data, const size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t) data[i];
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

uint32_t net_map_hash(const cJSON *map, char **json_out, size_t *size_out) {
    // Maps are immutable once loaded: remember the hash and size per object.
    static struct { const cJSON *map; uint32_t hash; size_t size; } cache[64];
    static size_t cache_next;

    if (json_out) *json_out = NULL;
    if (size_out) *size_out = 0;
    if (!map) return 0;

    if (!json_out) {
        for (size_t i = 0; i < sizeof(cache) / sizeof(cache[0]); i++) {
            if (cache[i].map != map) continue;
            if (size_out) *size_out = cache[i].size;
            return cache[i].hash;
        }
    }

    char *json = cJSON_PrintUnformatted(map);
    if (!json) return 0;

    const size_t size = strlen(json);
    const uint32_t hash = net_fnv1a(json, size);

    cache[cache_next].map = map;
    cache[cache_next].hash = hash;
    cache[cache_next].size = size;
    cache_next = (cache_next + 1) % (sizeof(cache) / sizeof(cache[0]));

    if (size_out) *size_out = size;
    if (json_out) *json_out = json;
    else cJSON_free(json);

    return hash;
}

int net_packet_read_input(NetInputPacket *input, const NetPacket *packet) {
    if (packet->length != sizeof(NetInputPacket) && packet->length != NET_INPUT_PACKET_V1_SIZE) return 0;

    input->slowdown = 0;
    memcpy(input, packet->payload, packet->length);
    return 1;
}

int net_packet_read_ent(NetEntPacket *ent, const NetPacket *packet) {
    if (packet->length != sizeof(NetEntPacket) && packet->length != NET_ENT_PACKET_V3_SIZE &&
        packet->length != NET_ENT_PACKET_V2_SIZE && packet->length != NET_ENT_PACKET_MIN_SIZE) return 0;

    ent->secondary_index = -1;
    ent->cosmetics = PLAYER_COSMETICS_NONE;
    ent->spectate = 0;
    memcpy(ent, packet->payload, packet->length);
    ent->spectate = ent->spectate ? 1 : 0;
    return 1;
}

int net_packet_read_spawn(NetSpawnPacket *spawn, const NetPacket *packet) {
    if (packet->length != sizeof(NetSpawnPacket) && packet->length != NET_SPAWN_PACKET_V1_SIZE) return 0;

    spawn->cosmetics = PLAYER_COSMETICS_NONE;
    memcpy(spawn, packet->payload, packet->length);
    return 1;
}

#ifndef KRUNKNATIVE_SERVER

void player_send_event() { /* SHIM */ }
void game_broadcast() { /* SHIM */ }

#endif
