#include <shared.h>
#include <string.h>

#ifdef WIN32
#include <winsock2.h>
#define NET_SEND(fd, data, len) send((fd), (const char*) (data), (int) (len), 0)
#define NET_ERRNO WSAGetLastError()
#define NET_WOULD_BLOCK(err) ((err) == WSAEWOULDBLOCK)
#else
#include <sys/socket.h>
#include <errno.h>
#define NET_SEND(fd, data, len) send((fd), (data), (len), 0)
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

#ifndef KRUNKNATIVE_SERVER

void player_send_event() { /* SHIM */ }
void game_broadcast() { /* SHIM */ }

#endif
