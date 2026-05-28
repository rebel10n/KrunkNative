#include <server.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef WIN32
#include <windows.h>
#define NET_ERRNO WSAGetLastError()
#define CLOSE_SOCKET closesocket
#define WOULD_BLOCK(err) ((err) == WSAEWOULDBLOCK)
#define SLEEP_MILLIS(ms) Sleep(ms)
#else
#include <unistd.h>
#define NET_ERRNO errno
#define CLOSE_SOCKET close
#define WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#define SLEEP_MILLIS(ms) do { struct timespec ts = {0, (ms) * 1000000L}; nanosleep(&ts, NULL); } while (0)
#endif

#define NET_ERROR(...) { replxx_print(g_replxx, __VA_ARGS__); g_server->should_quit = 1; return; }

static int net_alloc_player_number() {
    const unsigned short limit = g_server->config.max_players ? g_server->config.max_players : 8;

    for (int i = 1; i <= limit && i < 32; i++) {
        const unsigned int bit = 1u << i;
        if (g_server->used_player_numbers & bit) continue;

        g_server->used_player_numbers |= bit;
        return i;
    }

    return 0;
}

static void net_release_player_number(ClientConnection *client) {
    if (!client || client->player_number <= 0 || client->player_number >= 32) return;

    g_server->used_player_numbers &= ~(1u << client->player_number);
    client->player_number = 0;
}

static void net_assign_guest_name(ClientConnection *client) {
    if (!client) return;

    snprintf(client->name, sizeof(client->name), "Guest_%d", client->player_number);
}

static void net_free_player(Player *player) {
    if (!player) return;

    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);
    free(player);
}

static void net_remove_game_player(Player *player) {
    if (!player) return;

    size_t index = SIZE_MAX;

    for (size_t i = 0; i < g_server->game.player_count; i++) {
        if (g_server->game.players[i] == player) {
            index = i;
            break;
        }
    }

    if (index == SIZE_MAX) {
        net_free_player(player);
        return;
    }

    if (index + 1 < g_server->game.player_count) {
        memmove(&g_server->game.players[index], &g_server->game.players[index + 1], (g_server->game.player_count - index - 1) * sizeof(Player*));
    }

    g_server->game.player_count--;

    if (!g_server->game.player_count) {
        free(g_server->game.players);
        g_server->game.players = NULL;
    } else {
        Player **new_players = realloc(g_server->game.players, g_server->game.player_count * sizeof(Player*));
        if (new_players) g_server->game.players = new_players;
    }

    net_free_player(player);
}

static void net_mark_client_spawned(ClientConnection *client) {
    if (!client || !client->player) return;

    client->spawned_at = g_server->now;
    client->last_significant_move_at = g_server->now;
    client->idle_position = client->player->position;
    client->has_idle_position = 1;
}

static void net_set_nonblocking(const int fd) {
#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(fd, FIONBIO, &non_blocking);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void net_send_init(ClientConnection *client) {
    const NetInitPacket packet = {
        .major = 1,
        .minor = 0,
        .patch = 0,
        .map_index = g_server->game.current_map_index,
        .mode_index = g_server->game.current_mode_index,
        .tick_rate = g_server->config.tick_rate,
        .client_id = client->id,
    };

    net_send_packet(client->fd, NET_PACKET_INIT, &packet, sizeof(packet));
}

static void net_send_spawn(ClientConnection *client, const Player *player, const int is_you) {
    NetSpawnPacket packet = {
        .uid = player->uid,
        .is_you = (uint8_t) is_you,
        .active = player->active,
        .class_index = player->class_index,
        .team = player->team,
        .health = player->health,
        .max_health = player->max_health,
        .position = {player->position.x, player->position.y, player->position.z},
        .direction = {player->direction.x, player->direction.y},
    };

    ClientConnection *owner = NULL;

    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].player == player) {
            owner = &g_server->clients[i];
            break;
        }
    }

    if (owner) {
        strncpy(packet.name, owner->name, sizeof(packet.name) - 1);
    }

    net_send_packet(client->fd, NET_PACKET_SPAWN, &packet, sizeof(packet));
}

static void net_broadcast_spawn(const Player *player) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        net_send_spawn(client, player, client->player == player);
    }
}

static void net_broadcast_spawn_except(const Player *player, const ClientConnection *skip) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client == skip) continue;
        net_send_spawn(client, player, client->player == player);
    }
}

static void net_send_existing_spawns(ClientConnection *client) {
    for (size_t i = 0; i < g_server->game.player_count; i++) {
        Player *player = g_server->game.players[i];
        if (!player || !player->active || player == client->player) continue;
        net_send_spawn(client, player, 0);
    }
}

static void net_spawn_client(ClientConnection *client) {
    if (!client->player) {
        Player *player = player_init(&g_server->game);
        if (!player) return;

        player->uid = client->id;
        client->player = player;
        game_players_add(&g_server->game, player);
    }

    client->player->class_index = client->class_index;
    player_spawn(client->player);
    net_mark_client_spawned(client);
    net_send_existing_spawns(client);
    net_broadcast_spawn(client->player);
}

static void net_free_players() {
    for (size_t i = 0; i < g_server->client_count; i++) {
        g_server->clients[i].player = NULL;
        g_server->clients[i].spawned_at = 0.0f;
        g_server->clients[i].has_idle_position = 0;
    }

    for (size_t i = 0; i < g_server->game.player_count; i++) {
        Player *player = g_server->game.players[i];
        net_free_player(player);
    }

    free(g_server->game.players);
    g_server->game.players = NULL;
    g_server->game.player_count = 0;
}

static void net_cycle_map() {
    if (!g_server->game.map_count) return;

    const int next_map = (g_server->game.current_map_index + 1) % (int) g_server->game.map_count;
    const int mode = g_server->game.current_mode_index;
    uint8_t *respawn_clients = NULL;

    if (g_server->client_count) {
        respawn_clients = calloc(g_server->client_count, sizeof(uint8_t));

        if (respawn_clients) {
            for (size_t i = 0; i < g_server->client_count; i++) {
                const ClientConnection *client = &g_server->clients[i];
                respawn_clients[i] = client->player && client->player->active;
            }
        }
    }

    net_free_players();
    game_init(&g_server->game, next_map, mode, 0);

    replxx_print(g_replxx, "Loaded map %d mode %d \n", g_server->game.current_map_index, g_server->game.current_mode_index);

    for (size_t i = 0; i < g_server->client_count; i++) {
        net_send_init(&g_server->clients[i]);
    }

    if (respawn_clients) {
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (respawn_clients[i]) net_spawn_client(&g_server->clients[i]);
        }

        free(respawn_clients);
    }
}

void net_add_client(ClientConnection *client) {
    if (g_server->config.max_players && g_server->client_count >= g_server->config.max_players) {
        replxx_print(g_replxx, "Rejecting client: server full (%zu/%u) \n", g_server->client_count, (unsigned int) g_server->config.max_players);
        CLOSE_SOCKET(client->fd);
        return;
    }

    ClientConnection *new_clients = realloc(g_server->clients, (g_server->client_count + 1) * sizeof(ClientConnection));

    if (!new_clients) {
        CLOSE_SOCKET(client->fd);
        return;
    }

    g_server->clients = new_clients;
    client->id = g_server->next_client_id++;
    client->player_number = net_alloc_player_number();

    if (!client->player_number) {
        CLOSE_SOCKET(client->fd);
        return;
    }

    net_assign_guest_name(client);
    client->connected_at = g_server->now;
    client->spawned_at = 0.0f;
    client->last_significant_move_at = g_server->now;
    client->has_idle_position = 0;
    client->joined = 0;
    client->class_index = 0;
    g_server->clients[g_server->client_count] = *client;

    ClientConnection *stored_client = &g_server->clients[g_server->client_count++];
    on_client_connect(stored_client);
    net_send_init(stored_client);
}

void net_remove_client(ClientConnection *client) {
    size_t index = SIZE_MAX;

    for (size_t i = 0; i < g_server->client_count; i++) {
        if (&g_server->clients[i] == client) {
            index = i;
            break;
        }
    }

    if (index == SIZE_MAX) return;

    on_client_disconnect(client);
    if (client->player) {
        client->player->active = 0;
        net_broadcast_spawn_except(client->player, client);
        net_remove_game_player(client->player);
        client->player = NULL;
    }
    net_release_player_number(client);
    CLOSE_SOCKET(client->fd);

    if (index + 1 < g_server->client_count) {
        memmove(&g_server->clients[index], &g_server->clients[index + 1], (g_server->client_count - index - 1) * sizeof(ClientConnection));
    }

    g_server->client_count--;

    if (!g_server->client_count) {
        free(g_server->clients);
        g_server->clients = NULL;
        return;
    }

    ClientConnection *new_clients = realloc(g_server->clients, g_server->client_count * sizeof(ClientConnection));
    if (new_clients) g_server->clients = new_clients;
}

static void net_handle_packet(ClientConnection *client, const NetPacket *packet) {
    switch (packet->type) {
        case NET_PACKET_HELLO:
            break;
        case NET_PACKET_ENT: {
            if (packet->length != sizeof(NetEntPacket)) break;

            NetEntPacket ent;
            memcpy(&ent, packet->payload, sizeof(ent));
            client->class_index = ent.class_index;
            net_spawn_client(client);
            break;
        }
        case NET_PACKET_INPUT: {
            if (packet->length != sizeof(NetInputPacket) || !client->player || !client->player->active) break;

            NetInputPacket net_input;
            Input input;

            memcpy(&net_input, packet->payload, sizeof(net_input));
            net_packet_to_input(&input, &net_input);
            player_queue_input(client->player, &input);
            break;
        }
        case NET_PACKET_CYCLE:
            if (!packet->length) net_cycle_map();
            break;
        default:
            on_client_event(client);
            break;
    }
}

int net_poll_client(ClientConnection *client) {
    char buffer[1024];
    int read;

    if ((read = recv(client->fd, buffer, sizeof(buffer), 0)) <= 0) {
        if (read < 0 && WOULD_BLOCK(NET_ERRNO)) return 1;

        net_remove_client(client);
        return 0;
    }

    if (!net_buffer_push(&client->read_buffer, buffer, (size_t) read)) {
        net_remove_client(client);
        return 0;
    }

    NetPacket packet;
    int result;

    while ((result = net_buffer_next(&client->read_buffer, &packet)) > 0) {
        net_handle_packet(client, &packet);
    }

    if (result < 0) {
        net_remove_client(client);
        return 0;
    }

    return 1;
}

void net_main() {

#ifdef WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    const int server_socket = (int) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket < 0) NET_ERROR("Failed to create server socket, quitting...");

    struct sockaddr_in listen_addr;

    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(g_server->config.listen_port);

    if (bind(server_socket, (struct sockaddr *) &listen_addr, sizeof(listen_addr)) < 0) {
        NET_ERROR("Failed to bind to port %d (is it in use?), quitting...", g_server->config.listen_port);
    }

    if (listen(server_socket, 5) < 0) NET_ERROR("Failed to listen on port %d, quitting...", g_server->config.listen_port);

    net_set_nonblocking(server_socket);

    replxx_print(g_replxx, "Listening on port %d \n", g_server->config.listen_port);

    while (!g_server->should_quit) {
        pthread_mutex_lock(&g_server->lock);

        for (size_t i = 0; i < g_server->client_count; i++) {
            if (!net_poll_client(&g_server->clients[i])) i--;
        }

        pthread_mutex_unlock(&g_server->lock);

        struct sockaddr_in client_addr;
#ifdef WIN32
        int client_addr_size = sizeof(client_addr);
#else
        socklen_t client_addr_size = sizeof(client_addr);
#endif

        const int client = (int) accept(server_socket, (struct  sockaddr *) &client_addr, &client_addr_size);

        if (client < 0) {
            if (WOULD_BLOCK(NET_ERRNO)) {
                SLEEP_MILLIS(1);
                continue;
            }

            break;
        }

        net_set_nonblocking(client);

        ClientConnection conn = {};

        conn.fd = client;
        conn.addr = client_addr;

        pthread_mutex_lock(&g_server->lock);
        net_add_client(&conn);
        pthread_mutex_unlock(&g_server->lock);
    }

    CLOSE_SOCKET(server_socket);
}

void net_check_idle_clients(const float now) {
    const float timeout = g_server->config.idle_timeout;
    if (timeout <= 0.0f) return;

    const float threshold = g_server->config.idle_move_threshold;
    const float threshold_sq = threshold * threshold;

    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        Player *player = client->player;

        if (!player || !player->active) {
            const float idle_since = client->spawned_at > 0.0f ? client->last_significant_move_at : client->connected_at;
            if (now - idle_since >= timeout) {
                replxx_print(g_replxx, "Kicking %s for inactivity before spawn \n", client->name);
                net_remove_client(client);
                i--;
            }
            continue;
        }

        if (!client->has_idle_position) {
            net_mark_client_spawned(client);
            continue;
        }

        const float dx = player->position.x - client->idle_position.x;
        const float dy = player->position.y - client->idle_position.y;
        const float dz = player->position.z - client->idle_position.z;
        const float dist_sq = dx * dx + dy * dy + dz * dz;

        if (dist_sq >= threshold_sq) {
            client->idle_position = player->position;
            client->last_significant_move_at = now;
        }

        if (now - client->last_significant_move_at >= timeout) {
            replxx_print(g_replxx, "Kicking %s for inactivity in game \n", client->name);
            net_remove_client(client);
            i--;
        }
    }
}

void net_broadcast_states() {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];

        for (size_t j = 0; j < g_server->game.player_count; j++) {
            Player *player = g_server->game.players[j];
            if (!player || !player->active) continue;

            NetStatePacket packet;
            net_packet_from_player(&packet, player);
            net_send_packet(client->fd, NET_PACKET_STATE, &packet, sizeof(packet));
        }
    }
}

void net_cleanup() {
#ifdef WIN32
    WSACleanup();
#endif
}
