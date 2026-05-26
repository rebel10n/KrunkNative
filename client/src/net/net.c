#ifdef WIN32
#include <winsock2.h>
#include <windows.h>
#define NET_ERRNO WSAGetLastError()
#define SLEEP_MILLIS(ms) Sleep(ms)
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#define NET_ERRNO errno
#define CLOSE_SOCKET close
#define WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#define SLEEP_MILLIS(ms) do { struct timespec ts = {0, (ms) * 1000000L}; nanosleep(&ts, NULL); } while (0)
#endif

#include <client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int client_net_packet_logging_enabled() {
    static int checked = 0;
    static int enabled = 0;

    if (!checked) {
        const char *value = getenv("KRUNKNATIVE_PACKET_LOG");
        enabled = value && value[0] && strcmp(value, "0") != 0;
        checked = 1;
    }

    return enabled;
}

static void client_net_log_bad_payload(const char *name, const NetPacketType type, const void *payload, const uint16_t length) {
    const uint8_t *bytes = payload;

    printf("{\"type\":\"%s\",\"id\":%u,\"length\":%u,\"payload\":\"", name, (unsigned int) type, (unsigned int) length);

    for (uint16_t i = 0; i < length; i++) {
        printf("%02X", bytes ? bytes[i] : 0);
    }

    printf("\"}");
}

#ifdef WIN32
static void client_net_enable_ansi_output() {
    static int checked = 0;
    if (checked) return;

    checked = 1;

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE) return;

    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode)) return;

    SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#endif

void client_net_log_packet(const char *direction, const NetPacketType type, const void *payload, const uint16_t length) {
    if (!client_net_packet_logging_enabled()) return;

#ifdef WIN32
    client_net_enable_ansi_output();
#endif

    const int incoming = direction && direction[0] == '<';
    const char *style = incoming ? "\x1b[48;2;255;106;25m\x1b[30m" : "\x1b[48;2;119;255;119m\x1b[30m";

    printf("%s %s \x1b[0m ", style, incoming ? "<=" : "=>");

    if (length && !payload) {
        client_net_log_bad_payload("null", type, payload, length);
        printf("\n");
        fflush(stdout);
        return;
    }

    switch (type) {
        case NET_PACKET_HELLO: {
            if (length != sizeof(NetHelloPacket)) {
                client_net_log_bad_payload("hello", type, payload, length);
                break;
            }

            NetHelloPacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"hello\",\"id\":%u,\"major\":%u,\"minor\":%u,\"patch\":%u}",
                   (unsigned int) type,
                   (unsigned int) packet.major,
                   (unsigned int) packet.minor,
                   (unsigned int) packet.patch);
            break;
        }
        case NET_PACKET_INIT: {
            if (length != sizeof(NetInitPacket)) {
                client_net_log_bad_payload("init", type, payload, length);
                break;
            }

            NetInitPacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"init\",\"id\":%u,\"major\":%u,\"minor\":%u,\"patch\":%u,\"map_index\":%d,\"mode_index\":%d,\"tick_rate\":%u,\"client_id\":%d}",
                   (unsigned int) type,
                   (unsigned int) packet.major,
                   (unsigned int) packet.minor,
                   (unsigned int) packet.patch,
                   packet.map_index,
                   packet.mode_index,
                   (unsigned int) packet.tick_rate,
                   packet.client_id);
            break;
        }
        case NET_PACKET_ENT: {
            if (length != sizeof(NetEntPacket)) {
                client_net_log_bad_payload("ent", type, payload, length);
                break;
            }

            NetEntPacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"ent\",\"id\":%u,\"class_index\":%d}",
                   (unsigned int) type,
                   packet.class_index);
            break;
        }
        case NET_PACKET_SPAWN: {
            if (length != sizeof(NetSpawnPacket)) {
                client_net_log_bad_payload("spawn", type, payload, length);
                break;
            }

            NetSpawnPacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"spawn\",\"id\":%u,\"uid\":%d,\"is_you\":%u,\"active\":%u,\"position\":[%g,%g,%g],\"direction\":[%g,%g]}",
                   (unsigned int) type,
                   packet.uid,
                   (unsigned int) packet.is_you,
                   (unsigned int) packet.active,
                   packet.position[0],
                   packet.position[1],
                   packet.position[2],
                   packet.direction[0],
                   packet.direction[1]);
            break;
        }
        case NET_PACKET_INPUT: {
            if (length != sizeof(NetInputPacket)) {
                client_net_log_bad_payload("input", type, payload, length);
                break;
            }

            NetInputPacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"input\",\"id\":%u,\"seq\":%d,\"move_dir\":%d,\"delta\":%g,\"x_dir\":%g,\"y_dir\":%g,\"flags\":%u,\"swap\":%u}",
                   (unsigned int) type,
                   packet.seq,
                   packet.move_dir,
                   packet.delta,
                   packet.x_dir,
                   packet.y_dir,
                   (unsigned int) packet.flags,
                   (unsigned int) packet.swap);
            break;
        }
        case NET_PACKET_STATE: {
            if (length != sizeof(NetStatePacket)) {
                client_net_log_bad_payload("state", type, payload, length);
                break;
            }

            NetStatePacket packet;
            memcpy(&packet, payload, sizeof(packet));

            printf("{\"type\":\"state\",\"id\":%u,\"uid\":%d,\"ack_seq\":%d,\"active\":%u,\"loadout_index\":%d,\"position\":[%g,%g,%g],\"velocity\":[%g,%g,%g],\"direction\":[%g,%g],\"health\":%g,\"aim_val\":%g,\"crouch_val\":%g}",
                   (unsigned int) type,
                   packet.uid,
                   packet.ack_seq,
                   (unsigned int) packet.active,
                   packet.loadout_index,
                   packet.position[0],
                   packet.position[1],
                   packet.position[2],
                   packet.velocity[0],
                   packet.velocity[1],
                   packet.velocity[2],
                   packet.direction[0],
                   packet.direction[1],
                   packet.health,
                   packet.aim_val,
                   packet.crouch_val);
            break;
        }
        default:
            client_net_log_bad_payload("unknown", type, payload, length);
            break;
    }

    printf("\n");
    fflush(stdout);
}

int client_net_send_packet(const int socket, const NetPacketType type, const void *payload, const uint16_t length) {
    client_net_log_packet("=>", type, payload, length);
    return net_send_packet(socket, type, payload, length);
}

void client_net_queue_packet(Client *client, const NetPacket *packet) {
    pthread_mutex_lock(&client->net_lock);

    NetPacket *new_packets = realloc(client->net_packets, (client->net_packet_count + 1) * sizeof(NetPacket));

    if (new_packets) {
        client->net_packets = new_packets;
        client->net_packets[client->net_packet_count++] = *packet;
    }

    pthread_mutex_unlock(&client->net_lock);
}

int net_poll(Client *client, const int socket, NetPacketBuffer *read_buffer) {
    char buffer[1024];
    int read;

    if ((read = recv(socket, buffer, sizeof(buffer), 0)) <= 0) {
#ifdef WIN32
        if (read < 0 && NET_ERRNO == WSAEWOULDBLOCK) return 1;
#else
        if (read < 0 && WOULD_BLOCK(NET_ERRNO)) return 1;
#endif

        return 0;
    }

    if (!net_buffer_push(read_buffer, buffer, (size_t) read)) return 0;

    NetPacket packet;
    int result;

    while ((result = net_buffer_next(read_buffer, &packet)) > 0) {
        client_net_log_packet("<=", packet.type, packet.payload, packet.length);
        client_net_queue_packet(client, &packet);
    }

    if (result < 0) return 0;

    return 1;
}

void net_main(NetMainArgs *args) {
#ifdef WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    struct sockaddr_in server_address;

    // TODO: better address parsing
    server_address.sin_addr.s_addr = inet_addr(args->address);
    server_address.sin_port = htons(NET_DEFAULT_PORT);
    server_address.sin_family = AF_INET;

    const int client_socket = (int) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (client_socket < 0 || connect(client_socket, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
        on_client_socket_error(args->client, NET_ERRNO);
        return;
    }

    pthread_mutex_lock(&args->client->net_lock);
    args->client->net_socket = client_socket;
    pthread_mutex_unlock(&args->client->net_lock);

#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(client_socket, FIONBIO, &non_blocking);
#else
    int flags = fcntl(client_socket, F_GETFL, 0);
    fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
#endif

    on_client_socket_connect(args->client);

    const NetHelloPacket hello = {1, 0, 0};
    client_net_send_packet(client_socket, NET_PACKET_HELLO, &hello, sizeof(hello));

    NetPacketBuffer read_buffer = {};

    while (!args->client->net_should_quit) {
        if (!net_poll(args->client, client_socket, &read_buffer)) break;
        SLEEP_MILLIS(1);
    }

    pthread_mutex_lock(&args->client->net_lock);
    args->client->net_connected = 0;
    args->client->net_socket = -1;
    pthread_mutex_unlock(&args->client->net_lock);

#ifdef WIN32
    closesocket(client_socket);
#else
    CLOSE_SOCKET(client_socket);
#endif

    on_client_socket_disconnect(args->client);
}
